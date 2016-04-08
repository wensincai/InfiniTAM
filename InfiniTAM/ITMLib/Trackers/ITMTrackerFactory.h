// Copyright 2014-2015 Isis Innovation Limited and the authors of InfiniTAM

#pragma once

#include <stdexcept>
#include <vector>

#include "CPU/ITMColorTracker_CPU.h"
#include "CPU/ITMDepthTracker_CPU.h"
#include "CPU/ITMExtendedTracker_CPU.h"
#include "CPU/ITMRenTracker_CPU.h"
#include "Interface/ITMCompositeTracker.h"
#include "Interface/ITMIMUTracker.h"
#include "Interface/ITMTracker.h"
#include "../Engines/LowLevel/Interface/ITMLowLevelEngine.h"
#include "../Utils/ITMLibSettings.h"

#ifndef COMPILE_WITHOUT_CUDA
#include "CUDA/ITMColorTracker_CUDA.h"
#include "CUDA/ITMDepthTracker_CUDA.h"
#include "CUDA/ITMExtendedTracker_CUDA.h"
#include "CUDA/ITMRenTracker_CUDA.h"
#endif

#ifdef COMPILE_WITH_METAL
#include "Metal/ITMDepthTracker_Metal.h"
#endif

#include "../../ORUtils/KeyValueConfig.h"

namespace ITMLib
{
	/**
	 * \brief An instance of this class can be used to construct trackers.
	 */
	template <typename TVoxel, typename TIndex>
	class ITMTrackerFactory
	{
	private:
		//#################### TYPEDEFS ####################
		typedef ITMTracker* MakerFunc(const Vector2i&,const Vector2i&,ITMLibSettings::DeviceType,const ORUtils::KeyValueConfig &,const ITMLowLevelEngine*,ITMIMUCalibrator*,ITMScene<TVoxel,TIndex>*);

		/// Tracker types
		typedef enum {
			//! Identifies a tracker based on colour image
			TRACKER_COLOR,
			//! Identifies a tracker based on depth image
			TRACKER_ICP,
			//! Identifies a tracker based on depth and color image with various extensions
			TRACKER_EXTENDED,
			//! Identifies a tracker based on depth image (Ren et al, 2012)
			TRACKER_REN,
			//! Identifies a tracker based on depth image and IMU measurement
			TRACKER_IMU,
		} TrackerType;

		struct Maker {
			const char *id;
			const char *description;
			TrackerType type;
			MakerFunc *make;

			Maker(const char *_id, const char *_desc, TrackerType _type, MakerFunc *_make)
			  : id(_id), description(_desc), type(_type), make(_make)
			{}
		};

		//#################### PRIVATE VARIABLES ####################
		/** A list of maker functions for the various tracker types. */
		std::vector<Maker> makers;

		//################## SINGLETON IMPLEMENTATION ##################
		/**
		 * \brief Constructs a tracker factory.
		 */
		ITMTrackerFactory(void)
		{
			makers.push_back(Maker("rgb", "Colour based tracker", TRACKER_COLOR, &MakeColourTracker));
			makers.push_back(Maker("icp", "Depth based ICP tracker", TRACKER_ICP, &MakeICPTracker));
			makers.push_back(Maker("extended", "Depth + colour based tracker", TRACKER_EXTENDED, &MakeExtendedTracker));
			makers.push_back(Maker("imuicp", "Combined IMU and depth based ICP tracker", TRACKER_IMU, &MakeIMUTracker));
			makers.push_back(Maker("ren", "Depth based SDF tracker", TRACKER_REN, &MakeRenTracker));
		}

	public:
		/**
		 * \brief Gets the singleton instance for the current set of template parameters.
		 */
		static ITMTrackerFactory& Instance()
		{
			static ITMTrackerFactory s_instance;
			return s_instance;
		}

		//################## PUBLIC MEMBER FUNCTIONS ##################
	public:
	/**
	 * \brief Makes a tracker of the type specified in the settings.
	 */
	ITMTracker *Make(const Vector2i & imgSize_rgb, const Vector2i & imgSize_d, const ITMLibSettings *settings, const ITMLowLevelEngine *lowLevelEngine,
		ITMIMUCalibrator *imuCalibrator, ITMScene<TVoxel,TIndex> *scene) const
	{
		ORUtils::KeyValueConfig cfg(settings->trackerConfig);
		int verbose = 0;
		if (cfg.getProperty("help") != NULL) if (verbose < 10) verbose = 10;

		ORUtils::KeyValueConfig::ChoiceList trackerOptions;
		for (int i = 0; (unsigned)i < makers.size(); ++i) {
			trackerOptions.addChoice(makers[i].id, makers[i].type);
		}
		int type = TRACKER_ICP;
		cfg.parseChoiceProperty("type", "type of tracker", type, trackerOptions, verbose);
		const Maker *maker = NULL;
		for (int i = 0; (unsigned)i < makers.size(); ++i) {
			if (makers[i].type == type) {
				maker = &(makers[i]);
				break;
			}
		}
		if (maker == NULL) DIEWITHEXCEPTION("Unknown tracker type");

		ITMTracker *ret = (*(maker->make))(imgSize_rgb, imgSize_d, settings->deviceType, cfg, lowLevelEngine, imuCalibrator, scene);
		if (ret->requiresColourRendering() && (!TVoxel::hasColorInformation)) {
			printf("Error: Color tracker requires a voxel type with colour information!\n");
		}

		return ret;
	}

	//#################### PUBLIC STATIC MEMBER FUNCTIONS ####################
	static std::vector<TrackerIterationType> parseLevelConfig(const char *str)
	{
		bool parseError = false;
		std::vector<TrackerIterationType> ret;
		for (int i = static_cast<int>(strlen(str))-1; i >= 0; --i) {
			switch (str[i]) {
			case 'r':
				ret.push_back(TRACKER_ITERATION_ROTATION);
				break;
			case 't':
				ret.push_back(TRACKER_ITERATION_TRANSLATION);
				break;
			case 'b':
				ret.push_back(TRACKER_ITERATION_BOTH);
				break;
			case 'n':
				ret.push_back(TRACKER_ITERATION_NONE);
				break;
			default:
				parseError = true;
				break;
			}
		}

		if (parseError) {
			fprintf(stderr, "error parsing level configuration '%s'\n", str);
			for (int i = 0; (unsigned)i < ret.size(); ++i) fprintf(stderr, "level %i: %i\n", (int)ret.size()-i, (int)(ret[ret.size()-i]));
		}
		return ret;
	}

	/**
	 * \brief Makes a colour tracker.
	 */
	static ITMTracker *MakeColourTracker(const Vector2i& imgSize_rgb, const Vector2i& imgSize_d, ITMLibSettings::DeviceType deviceType, const ORUtils::KeyValueConfig & cfg,
		const ITMLowLevelEngine *lowLevelEngine, ITMIMUCalibrator *imuCalibrator, ITMScene<TVoxel,TIndex> *scene)
	{
		int verbose = 0;
		if (cfg.getProperty("help") != NULL) if (verbose < 10) verbose = 10;

		const char *levelSetup = "rrrbb";
		cfg.parseStrProperty("levels", "resolution hierarchy levels", levelSetup, verbose);
		std::vector<TrackerIterationType> levels = parseLevelConfig(levelSetup);

		ITMColorTracker *ret = NULL;
		switch (deviceType)
		{
		case ITMLibSettings::DEVICE_CPU:
			ret = new ITMColorTracker_CPU(imgSize_rgb, &(levels[0]), static_cast<int>(levels.size()), lowLevelEngine);
			break;
		case ITMLibSettings::DEVICE_CUDA:
#ifndef COMPILE_WITHOUT_CUDA
			ret = new ITMColorTracker_CUDA(imgSize_rgb, &(levels[0]), static_cast<int>(levels.size()), lowLevelEngine);
#endif
			break;
		case ITMLibSettings::DEVICE_METAL:
#ifdef COMPILE_WITH_METAL
			ret = new ITMColorTracker_CPU(imgSize_rgb, &(levels[0]), static_cast<int>(levels.size()), lowLevelEngine);
#endif
			break;
        	}

		if (ret==NULL) DIEWITHEXCEPTION("Failed to make colour tracker");
		return ret;
	}

	/**
	 * \brief Makes an ICP tracker.
	 */
	static ITMTracker *MakeICPTracker(const Vector2i& imgSize_rgb, const Vector2i& imgSize_d, ITMLibSettings::DeviceType deviceType, const ORUtils::KeyValueConfig & cfg,
		const ITMLowLevelEngine *lowLevelEngine, ITMIMUCalibrator *imuCalibrator, ITMScene<TVoxel,TIndex> *scene)
	{
		const char *levelSetup = "rrrbb";
		float smallStepSizeCriterion = 1e-3f;
		float outlierDistanceFine = 0.002f;
		float outlierDistanceCoarse = 0.01f;
		float failureDetectorThd = 3.0f;
		int numIterationsCoarse = 10;
		int numIterationsFine = 2;

		int verbose = 0;
		if (cfg.getProperty("help") != NULL) if (verbose < 10) verbose = 10;
		cfg.parseStrProperty("levels", "resolution hierarchy levels", levelSetup, verbose);
		std::vector<TrackerIterationType> levels = parseLevelConfig(levelSetup);

		cfg.parseFltProperty("minstep", "step size threshold for convergence", smallStepSizeCriterion, verbose);
		cfg.parseFltProperty("outlierC", "outlier threshold at coarsest level", outlierDistanceCoarse, verbose);
		cfg.parseFltProperty("outlierF", "outlier threshold at finest level", outlierDistanceFine, verbose);
		cfg.parseIntProperty("numiterC", "maximum number of iterations at coarsest level", numIterationsCoarse, verbose);
		cfg.parseIntProperty("numiterF", "maximum number of iterations at finest level", numIterationsFine, verbose);
		cfg.parseFltProperty("failureDec", "threshold for the failure detection", failureDetectorThd, verbose);

		ITMDepthTracker *ret = NULL;
		switch (deviceType)
		{
		case ITMLibSettings::DEVICE_CPU:
			ret = new ITMDepthTracker_CPU(imgSize_d, &(levels[0]), static_cast<int>(levels.size()), smallStepSizeCriterion, failureDetectorThd, lowLevelEngine);
			break;
		case ITMLibSettings::DEVICE_CUDA:
#ifndef COMPILE_WITHOUT_CUDA
			ret = new ITMDepthTracker_CUDA(imgSize_d, &(levels[0]), static_cast<int>(levels.size()), smallStepSizeCriterion, failureDetectorThd, lowLevelEngine);
#endif
			break;
		case ITMLibSettings::DEVICE_METAL:
#ifdef COMPILE_WITH_METAL
			ret = new ITMDepthTracker_Metal(imgSize_d, &(levels[0]), static_cast<int>(levels.size()), smallStepSizeCriterion, failureDetectorThd, lowLevelEngine);
#endif
			break;
		}

		if (ret == NULL) DIEWITHEXCEPTION("Failed to make ICP tracker");
		ret->SetupLevels(numIterationsCoarse, numIterationsFine,
			outlierDistanceCoarse, outlierDistanceFine);
		return ret;
	}

	/**
	* \brief Makes an Extended tracker.
	*/
	static ITMTracker *MakeExtendedTracker(const Vector2i& imgSize_rgb, const Vector2i& imgSize_d, ITMLibSettings::DeviceType deviceType, const ORUtils::KeyValueConfig & cfg,
		const ITMLowLevelEngine *lowLevelEngine, ITMIMUCalibrator *imuCalibrator, ITMScene<TVoxel, TIndex> *scene)
	{
		const char *levelSetup = "rrbb";
		float smallStepSizeCriterion = 1e-4f;
		float outlierSpaceDistanceFine = 0.002f;
		float outlierSpaceDistanceCoarse = 0.01f;
		float failureDetectorThd = 3.0f;
		int numIterationsCoarse = 10;
		int numIterationsFine = 2;

		int verbose = 0;
		if (cfg.getProperty("help") != NULL) if (verbose < 10) verbose = 10;
		cfg.parseStrProperty("levels", "resolution hierarchy levels", levelSetup, verbose);
		std::vector<TrackerIterationType> levels = parseLevelConfig(levelSetup);

		cfg.parseFltProperty("minstep", "step size threshold for convergence", smallStepSizeCriterion, verbose);
		cfg.parseFltProperty("outlierSpaceC", "space outlier threshold at coarsest level", outlierSpaceDistanceCoarse, verbose);
		cfg.parseFltProperty("outlierSpaceF", "space outlier threshold at finest level", outlierSpaceDistanceFine, verbose);
		cfg.parseIntProperty("numiterC", "maximum number of iterations at coarsest level", numIterationsCoarse, verbose);
		cfg.parseIntProperty("numiterF", "maximum number of iterations at finest level", numIterationsFine, verbose);
		cfg.parseFltProperty("failureDec", "threshold for the failure detection", failureDetectorThd, verbose);

		ITMExtendedTracker *ret = NULL;
		switch (deviceType)
		{
		case ITMLibSettings::DEVICE_CPU:
			ret = new ITMExtendedTracker_CPU(imgSize_d, &(levels[0]), static_cast<int>(levels.size()), smallStepSizeCriterion, failureDetectorThd, lowLevelEngine);
			break;
		case ITMLibSettings::DEVICE_CUDA:
#ifndef COMPILE_WITHOUT_CUDA
			ret = new ITMExtendedTracker_CUDA(imgSize_d, &(levels[0]), static_cast<int>(levels.size()), smallStepSizeCriterion, failureDetectorThd, lowLevelEngine);
#endif
			break;
		case ITMLibSettings::DEVICE_METAL:
#ifdef COMPILE_WITH_METAL
			ret = new ITMExtendedTracker_Metal(imgSize_d, &(levels[0]), static_cast<int>(levels.size()), smallStepSizeCriterion, failureDetectorThd, lowLevelEngine);
#endif
			break;
		}

		if (ret == NULL) DIEWITHEXCEPTION("Failed to make extended tracker");
		ret->SetupLevels(numIterationsCoarse, numIterationsFine, outlierSpaceDistanceCoarse, outlierSpaceDistanceFine);
		return ret;
	}
	
	/**
	 * \brief Makes an IMU tracker.
	 */
	static ITMTracker* MakeIMUTracker(const Vector2i& imgSize_rgb, const Vector2i& imgSize_d, ITMLibSettings::DeviceType deviceType, const ORUtils::KeyValueConfig & cfg,
		const ITMLowLevelEngine *lowLevelEngine, ITMIMUCalibrator *imuCalibrator, ITMScene<TVoxel,TIndex> *scene)
	{
		const char *levelSetup = "tb";
		float smallStepSizeCriterion = 1e-3f;
		float outlierDistanceFine = 0.005f;
		float outlierDistanceCoarse = 0.01f;
		float failureDetectorThd = 3.0f;
		int numIterationsCoarse = 4;
		int numIterationsFine = 2;

		int verbose = 0;
		if (cfg.getProperty("help") != NULL) if (verbose < 10) verbose = 10;
		cfg.parseStrProperty("levels", "resolution hierarchy levels", levelSetup, verbose);
		std::vector<TrackerIterationType> levels = parseLevelConfig(levelSetup);

		cfg.parseFltProperty("minstep", "step size threshold for convergence", smallStepSizeCriterion, verbose);
		cfg.parseFltProperty("outlierC", "outlier threshold at coarsest level", outlierDistanceCoarse, verbose);
		cfg.parseFltProperty("outlierF", "outlier threshold at finest level", outlierDistanceFine, verbose);
		cfg.parseIntProperty("numiterC", "maximum number of iterations at coarsest level", numIterationsCoarse, verbose);
		cfg.parseIntProperty("numiterF", "maximum number of iterations at finest level", numIterationsFine, verbose);
		cfg.parseFltProperty("failureDec", "threshold for the failure detection", failureDetectorThd, verbose);

		ITMDepthTracker *dTracker = NULL;
		switch (deviceType)
		{
		case ITMLibSettings::DEVICE_CPU:
			dTracker = new ITMDepthTracker_CPU(imgSize_d, &(levels[0]), static_cast<int>(levels.size()), smallStepSizeCriterion, failureDetectorThd, lowLevelEngine);
			break;
		case ITMLibSettings::DEVICE_CUDA:
#ifndef COMPILE_WITHOUT_CUDA
			dTracker = new ITMDepthTracker_CUDA(imgSize_d, &(levels[0]), static_cast<int>(levels.size()), smallStepSizeCriterion, failureDetectorThd, lowLevelEngine);
#endif
			break;
		case ITMLibSettings::DEVICE_METAL:
#ifdef COMPILE_WITH_METAL
			dTracker = new ITMDepthTracker_Metal(imgSize_d, &(levels[0]), static_cast<int>(levels.size()), smallStepSizeCriterion, failureDetectorThd, lowLevelEngine);
#endif
			break;
		default: break;
		}

		if (dTracker == NULL) DIEWITHEXCEPTION("Failed to make IMU tracker");
		dTracker->SetupLevels(numIterationsCoarse, numIterationsFine,
			outlierDistanceCoarse, outlierDistanceFine);

		ITMCompositeTracker *compositeTracker = new ITMCompositeTracker(2);
		compositeTracker->SetTracker(new ITMIMUTracker(imuCalibrator), 0);
		compositeTracker->SetTracker(dTracker, 1);
		return compositeTracker;
	}

	/**
	 * \brief Makes a Ren tracker.
	 */
	static ITMTracker *MakeRenTracker(const Vector2i& imageSize_rgb, const Vector2i& imgSize_d, ITMLibSettings::DeviceType deviceType, const ORUtils::KeyValueConfig & cfg,
		const ITMLowLevelEngine *lowLevelEngine, ITMIMUCalibrator *imuCalibrator, ITMScene<TVoxel,TIndex> *scene)
	{
		const char *levelSetup = "bb";

		int verbose = 0;
		if (cfg.getProperty("help") != NULL) if (verbose < 10) verbose = 10;
		cfg.parseStrProperty("levels", "resolution hierarchy levels", levelSetup, verbose);
		std::vector<TrackerIterationType> levels = parseLevelConfig(levelSetup);

		switch(deviceType)
		{
		case ITMLibSettings::DEVICE_CPU:
			return new ITMRenTracker_CPU<TVoxel, TIndex>(imgSize_d, &(levels[0]), static_cast<int>(levels.size()), lowLevelEngine, scene);
		case ITMLibSettings::DEVICE_CUDA:
#ifndef COMPILE_WITHOUT_CUDA
			return new ITMRenTracker_CUDA<TVoxel, TIndex>(imgSize_d, &(levels[0]), static_cast<int>(levels.size()), lowLevelEngine, scene);
#else
			break;
#endif
		case ITMLibSettings::DEVICE_METAL:
#ifdef COMPILE_WITH_METAL
			return new ITMRenTracker_CPU<TVoxel, TIndex>(imgSize_d, &(levels[0]), static_cast<int>(levels.size()), lowLevelEngine, scene);
#else
			break;
#endif
		default: break;
		}

		DIEWITHEXCEPTION("Failed to make Ren tracker");
	}
};

}
