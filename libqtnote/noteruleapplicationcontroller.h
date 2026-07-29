#ifndef NOTERULEAPPLICATIONCONTROLLER_H
#define NOTERULEAPPLICATIONCONTROLLER_H

#include "draftstore.h"
#include "noterule.h"
#include "qtnote_export.h"

#include <QHash>
#include <QObject>

namespace QtNote {

class DraftManager;
class FolderCatalogManager;
class FolderOperationsController;
class NoteManager;
class NoteRuleManager;

/**
 * Applies routing rules only at the draft publication boundary. This prevents
 * merely reading a remote note from changing it or creating cross-device
 * rule loops. The controller mutates the persisted draft before DraftManager
 * starts any storage operation, then records successful rule outcomes after
 * publication is acknowledged.
 */
class QTNOTE_EXPORT NoteRuleApplicationController final : public QObject {
    Q_OBJECT
public:
    static NoteRuleApplicationController *instance();

    explicit NoteRuleApplicationController(NoteRuleManager *ruleManager = nullptr,
                                           FolderCatalogManager *folderCatalogManager = nullptr,
                                           NoteManager *noteManager = nullptr, DraftManager *draftManager = nullptr,
                                           QObject *parent = nullptr);
    ~NoteRuleApplicationController() override;

    void initialize();
    bool isInitialized() const { return initialized_; }

signals:
    void ruleApplicationFailed(const QString &storageId, const QString &noteId, const QString &message);

private:
    struct PendingMarkers {
        NoteRuleEvaluationInput input;
        QList<QUuid>            ruleIds;
    };

    NoteRuleManager            *ruleManager_ { nullptr };
    FolderCatalogManager       *folderCatalogManager_ { nullptr };
    NoteManager                *noteManager_ { nullptr };
    DraftManager               *draftManager_ { nullptr };
    FolderOperationsController *folderOperations_ { nullptr };
    QHash<QUuid, PendingMarkers> publicationMarkers_;
    QHash<QString, PendingMarkers> pendingMarkers_;
    bool markerFlushScheduled_ { false };
    bool initialized_ { false };

    DraftStoreError          routeDraft(DraftRecord *record);
    void                     handleDraftPublished(const QUuid &draftId, const Note &note);
    void                     flushMarkers();
    void                     enqueueMarkers(const QList<QUuid> &ruleIds, const NoteRuleEvaluationInput &input);
    void                     reportFailure(const QString &storageId, const QString &noteId, const QString &message);
    NoteRuleEvaluation       evaluateRules(const NoteRuleEvaluationInput &input) const;
    NoteRuleEvaluationInput  evaluationInput(const DraftRecord &record) const;
    static QString           markerBatchKey(const NoteRuleEvaluationInput &input);
};

} // namespace QtNote

#endif // NOTERULEAPPLICATIONCONTROLLER_H
