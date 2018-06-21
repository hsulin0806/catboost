#include "docs_importance_helpers.h"
#include "ders_helpers.h"

#include <catboost/libs/algo/index_calcer.h>

TVector<TVector<double>> TDocumentImportancesEvaluator::GetDocumentImportances(const TPool& pool) {
    NPar::TLocalExecutor localExecutor;
    localExecutor.RunAdditionalThreads(ThreadCount - 1);

    TVector<TVector<ui32>> leafIndices(TreeCount);
    const TVector<ui8> binarizedFeatures = BinarizeFeatures(Model, pool);
    localExecutor.ExecRange([&] (int treeId) {
        leafIndices[treeId] = BuildIndicesForBinTree(Model, binarizedFeatures, treeId);
    }, NPar::TLocalExecutor::TExecRangeParams(0, TreeCount), NPar::TLocalExecutor::WAIT_COMPLETE);

    UpdateFinalFirstDerivatives(leafIndices, pool);
    TVector<TVector<double>> documentImportances(DocCount, TVector<double>(pool.Docs.GetDocCount()));

    localExecutor.ExecRange([&] (int docId) {
        // The derivative of leaf values with respect to train doc weight.
        TVector<TVector<TVector<double>>> leafDerivatives(TreeCount, TVector<TVector<double>>(LeavesEstimationIterations)); // [treeCount][LeavesEstimationIterationsCount][leafCount]
        UpdateLeavesDerivatives(docId, &leafDerivatives);
        GetDocumentImportancesForOneTrainDoc(leafDerivatives, leafIndices, &documentImportances[docId]);
    }, NPar::TLocalExecutor::TExecRangeParams(0, DocCount), NPar::TLocalExecutor::WAIT_COMPLETE);
    return documentImportances;
}

void TDocumentImportancesEvaluator::UpdateFinalFirstDerivatives(const TVector<TVector<ui32>>& leafIndices, const TPool& pool) {
    const ui32 docCount = pool.Docs.GetDocCount();
    TVector<double> finalApproxes(docCount);

    for (ui32 treeId = 0; treeId < TreeCount; ++treeId) {
        const TVector<ui32>& leafIndicesRef = leafIndices[treeId];
        for (ui32 it = 0; it < LeavesEstimationIterations; ++it) {
            const TVector<double>& leafValues = TreesStatistics[treeId].LeafValues[it];
            for (ui32 docId = 0; docId < docCount; ++docId) {
                finalApproxes[docId] += leafValues[leafIndicesRef[docId]];
            }
        }
    }

    FinalFirstDerivatives.resize(docCount);
    EvaluateDerivatives(LossFunction, LeafEstimationMethod, finalApproxes, pool, &FinalFirstDerivatives, nullptr, nullptr);
}

TVector<ui32> TDocumentImportancesEvaluator::GetLeafIdToUpdate(ui32 treeId, const TVector<double>& jacobian) {
    TVector<ui32> leafIdToUpdate;
    const ui32 leafCount = 1 << Model.ObliviousTrees.TreeSizes[treeId];

    if (UpdateMethod.UpdateType == EUpdateType::AllPoints) {
        leafIdToUpdate.resize(leafCount);
        std::iota(leafIdToUpdate.begin(), leafIdToUpdate.end(), 0);
    } else if (UpdateMethod.UpdateType == EUpdateType::TopKLeaves) {
        const TVector<ui32>& leafIndices = TreesStatistics[treeId].LeafIndices;
        TVector<double> leafJacobians(leafCount);
        for (ui32 docId = 0; docId < DocCount; ++docId) {
            leafJacobians[leafIndices[docId]] += Abs(jacobian[docId]);
        }

        TVector<ui32> orderedLeafIndices(leafCount);
        std::iota(orderedLeafIndices.begin(), orderedLeafIndices.end(), 0);
        Sort(orderedLeafIndices.begin(), orderedLeafIndices.end(), [&](ui32 firstDocId, ui32 secondDocId) {
            return leafJacobians[firstDocId] > leafJacobians[secondDocId];
        });

        leafIdToUpdate = TVector<ui32>(
            orderedLeafIndices.begin(),
            orderedLeafIndices.begin() + Min<ui32>(UpdateMethod.TopSize, leafCount)
        );
    }

    return leafIdToUpdate;
}

void TDocumentImportancesEvaluator::UpdateLeavesDerivatives(ui32 removedDocId, TVector<TVector<TVector<double>>>* leafDerivatives) {
    TVector<double> jacobian(DocCount);
    for (ui32 treeId = 0; treeId < TreeCount; ++treeId) {
        auto& treeStatistics = TreesStatistics[treeId];
        for (ui32 it = 0; it < LeavesEstimationIterations; ++it) {
            const TVector<ui32> leafIdToUpdate = GetLeafIdToUpdate(treeId, jacobian);
            TVector<double>& leafDerivativesRef = (*leafDerivatives)[treeId][it];

            // Updating Leaves Derivatives
            UpdateLeavesDerivativesForTree(
                leafIdToUpdate,
                removedDocId,
                jacobian,
                treeId,
                it,
                &leafDerivativesRef
            );

            // Updating Jacobian
            bool isRemovedDocUpdated = false;
            for (ui32 leafId : leafIdToUpdate) {
                for (ui32 docId : treeStatistics.LeavesDocId[leafId]) {
                    jacobian[docId] += leafDerivativesRef[leafId];
                }
                isRemovedDocUpdated |= (treeStatistics.LeafIndices[removedDocId] == leafId);
            }
            if (!isRemovedDocUpdated) {
                ui32 removedDocLeafId = treeStatistics.LeafIndices[removedDocId];
                jacobian[removedDocId] += leafDerivativesRef[removedDocLeafId];
            }
        }
    }
}

void TDocumentImportancesEvaluator::GetDocumentImportancesForOneTrainDoc(
    const TVector<TVector<TVector<double>>>& leafDerivatives,
    const TVector<TVector<ui32>>& leafIndices,
    TVector<double>* documentImportance
) {
    const ui32 docCount = documentImportance->size();
    TVector<double> predictedDerivatives(docCount);

    for (ui32 treeId = 0; treeId < TreeCount; ++treeId) {
        const TVector<ui32>& leafIndicesRef = leafIndices[treeId];
        for (ui32 it = 0; it < LeavesEstimationIterations; ++it) {
            const TVector<double>& leafDerivativesRef = leafDerivatives[treeId][it];
            for (ui32 docId = 0; docId < docCount; ++docId) {
                predictedDerivatives[docId] += leafDerivativesRef[leafIndicesRef[docId]];
            }
        }
    }

    for (ui32 docId = 0; docId < docCount; ++docId) {
        (*documentImportance)[docId] = FinalFirstDerivatives[docId] * predictedDerivatives[docId];
    }
}

void TDocumentImportancesEvaluator::UpdateLeavesDerivativesForTree(
    const TVector<ui32>& leafIdToUpdate,
    ui32 removedDocId,
    const TVector<double>& jacobian,
    ui32 treeId,
    ui32 leavesEstimationIteration,
    TVector<double>* leafDerivatives
) {
    auto& leafDerivativesRef = *leafDerivatives;
    const auto& treeStatistics = TreesStatistics[treeId];
    const TVector<double>& formulaNumeratorMultiplier = treeStatistics.FormulaNumeratorMultiplier[leavesEstimationIteration];
    const TVector<double>& formulaNumeratorAdding = treeStatistics.FormulaNumeratorAdding[leavesEstimationIteration];
    const TVector<double>& formulaDenominators = treeStatistics.FormulaDenominators[leavesEstimationIteration];
    const ui32 removedDocLeafId = treeStatistics.LeafIndices[removedDocId];

    leafDerivativesRef.resize(treeStatistics.LeafCount);
    Fill(leafDerivativesRef.begin(), leafDerivativesRef.end(), 0);
    bool isRemovedDocUpdated = false;
    for (ui32 leafId : leafIdToUpdate) {
        for (ui32 docId : treeStatistics.LeavesDocId[leafId]) {
            leafDerivativesRef[leafId] += formulaNumeratorMultiplier[docId] * jacobian[docId];
        }
        if (leafId == removedDocLeafId) {
            leafDerivativesRef[leafId] += formulaNumeratorAdding[removedDocId];
        }
        leafDerivativesRef[leafId] *= -LearningRate / formulaDenominators[leafId];
        isRemovedDocUpdated |= (leafId == removedDocLeafId);
    }
    if (!isRemovedDocUpdated) {
        leafDerivativesRef[removedDocLeafId] += jacobian[removedDocId] * formulaNumeratorMultiplier[removedDocId];
        leafDerivativesRef[removedDocLeafId] += formulaNumeratorAdding[removedDocId];
        leafDerivativesRef[removedDocLeafId] *= -LearningRate / formulaDenominators[removedDocLeafId];
    }
}
