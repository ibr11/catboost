#pragma once

#include "train.h"
#include <catboost/cuda/cuda_lib/cuda_base.h>
#include <catboost/libs/overfitting_detector/error_tracker.h>
#include <catboost/libs/loggers/logger.h>
#include <catboost/cuda/methods/boosting_progress_tracker.h>

#include <library/threading/local_executor/local_executor.h>

namespace NCatboostCuda {
    template <class TBoosting>
    inline TBoosting MakeBoosting(
        const NCatboostOptions::TCatBoostOptions& catBoostOptions,
        TBinarizedFeaturesManager* featureManager,
        TGpuAwareRandom* random,
        NPar::TLocalExecutor* localExecutor
    ) {
        TBoosting boosting(*featureManager,
                           catBoostOptions,
                           catBoostOptions.DataProcessingOptions->GpuCatFeaturesStorage,
                           *random,
                           localExecutor);
        return boosting;
    }

    template <class TWeakLearner>
    inline TWeakLearner MakeWeakLearner(TBinarizedFeaturesManager& featureManager,
        const NCatboostOptions::TCatBoostOptions& catBoostOptions
    ) {
        const bool zeroAverage = catBoostOptions.LossFunctionDescription->GetLossFunction() == ELossFunction::PairLogit;
        return TWeakLearner(featureManager, catBoostOptions, zeroAverage);
    }

    inline TBoostingProgressTracker MakeBoostingProgressTracker(
        const TTrainModelInternalOptions& internalOptions,
        const NCatboostOptions::TCatBoostOptions& catBoostOptions,
        const NCatboostOptions::TOutputFilesOptions& outputOptions,
        const NCB::TTrainingDataProvider* test,
        ui32 approxDimension,
        const TMaybe<TOnEndIterationCallback>& onEndIterationCallback,
        bool hasWeights
    ) {
        return TBoostingProgressTracker(catBoostOptions,
            outputOptions,
            internalOptions.ForceCalcEvalMetricOnEveryIteration,
            test != nullptr,
            /*testHasTarget*/ (test != nullptr) && test->MetaInfo.HasTarget,
            approxDimension,
            onEndIterationCallback,
            hasWeights);
    }

    template <class TBoosting>
    inline THolder<TAdditiveModel<typename TBoosting::TWeakModel>> Train(TBinarizedFeaturesManager& featureManager,
                                                                         const TTrainModelInternalOptions& internalOptions,
                                                                         const NCatboostOptions::TCatBoostOptions& catBoostOptions,
                                                                         const NCatboostOptions::TOutputFilesOptions& outputOptions,
                                                                         const NCB::TTrainingDataProvider& learn,
                                                                         const NCB::TTrainingDataProvider* test,
                                                                         const NCB::TFeatureEstimators& featureEstimators,
                                                                         TGpuAwareRandom& random,
                                                                         ui32 approxDimension,
                                                                         const TMaybe<TOnEndIterationCallback>& onEndIterationCallback,
                                                                         NPar::TLocalExecutor* localExecutor,
                                                                         TVector<TVector<double>>* testMultiApprox, // [dim][docIdx]
                                                                         TMetricsAndTimeLeftHistory* metricsAndTimeHistory) {
        auto boosting = MakeBoosting<TBoosting>(catBoostOptions, &featureManager, &random, localExecutor);

        boosting.SetDataProvider(learn, featureEstimators, test);

        auto progressTracker = MakeBoostingProgressTracker(internalOptions, catBoostOptions, outputOptions, test, approxDimension, onEndIterationCallback, learn.MetaInfo.HasWeights);

        boosting.SetBoostingProgressTracker(&progressTracker);

        auto model = boosting.Run();

        if (progressTracker.EvalMetricWasCalculated()) {
            const auto& errorTracker = progressTracker.GetErrorTracker();
            CATBOOST_NOTICE_LOG << "bestTest = " << errorTracker.GetBestError() << Endl;
            CATBOOST_NOTICE_LOG << "bestIteration = " << errorTracker.GetBestIteration() << Endl;
            if (outputOptions.ShrinkModelToBestIteration()) {
                *testMultiApprox = progressTracker.GetBestTestCursor();
            } else {
                *testMultiApprox = progressTracker.GetFinalTestCursor();
            }
        }

        if (outputOptions.ShrinkModelToBestIteration()) {
            if (test == nullptr) {
                CATBOOST_INFO_LOG << "Warning: can't use-best-model without test set. Will skip model shrinking";
            } else if (!progressTracker.EvalMetricWasCalculated()) {
                CATBOOST_INFO_LOG << "Warning: can't use-best-model because eval metric was not calculated "
                                     "due to the absence of target data in test set. Will skip model shrinking";
            } else {
                progressTracker.ShrinkToBestIteration(model.Get());
            }
        }

        if (metricsAndTimeHistory) {
            *metricsAndTimeHistory = progressTracker.GetMetricsAndTimeLeftHistory();
        }

        return model;
    }

    template <class TBoosting>
    inline void ModelBasedEval(TBinarizedFeaturesManager& featureManager,
                               const NCatboostOptions::TCatBoostOptions& catBoostOptions,
                               const NCatboostOptions::TOutputFilesOptions& outputOptions,
                               const NCB::TTrainingDataProvider& learn,
                               const NCB::TTrainingDataProvider& test,
                               TGpuAwareRandom& random,
                               ui32 approxDimension,
                               NPar::TLocalExecutor* localExecutor) {
        auto boosting = MakeBoosting<TBoosting>(catBoostOptions, &featureManager, &random, localExecutor);

        //TODO(noxoomo): support estimators in MBE
        NCB::TFeatureEstimators estimators;
        boosting.SetDataProvider(learn, estimators, &test);

        auto progressTracker = MakeBoostingProgressTracker(TTrainModelInternalOptions(), catBoostOptions, outputOptions, &test, approxDimension, Nothing(), learn.MetaInfo.HasWeights);

        boosting.SetBoostingProgressTracker(&progressTracker);

        boosting.RunModelBasedEval();
    }
}
