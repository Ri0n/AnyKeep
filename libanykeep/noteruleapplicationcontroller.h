#ifndef NOTERULEAPPLICATIONCONTROLLER_H
#define NOTERULEAPPLICATIONCONTROLLER_H

#include "draftstore.h"
#include "noterule.h"
#include "anykeep_export.h"

#include <QHash>
#include <QObject>
#include <QSet>

namespace AnyKeep {

class DraftManager;
class FolderCatalogManager;
class FolderOperationsController;
class NoteManager;
class NoteRuleManager;
class NoteStorage;

/**
 * Applies routing rules at the draft publication boundary. This prevents
 * merely reading a remote note from changing it or creating cross-device
 * rule loops. The only separate path is an explicit provider opt-in for a
 * local folder-overlay import; it cannot perform provider writes or storage
 * routing. The controller mutates the persisted draft before DraftManager
 * starts any storage operation, then records successful rule outcomes after
 * publication is acknowledged.
 */
class ANYKEEP_EXPORT NoteRuleApplicationController final : public QObject {
    Q_OBJECT
public:
    static NoteRuleApplicationController *instance();

    explicit NoteRuleApplicationController(NoteRuleManager      *ruleManager          = nullptr,
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

    NoteRuleManager               *ruleManager_ { nullptr };
    FolderCatalogManager          *folderCatalogManager_ { nullptr };
    NoteManager                   *noteManager_ { nullptr };
    DraftManager                  *draftManager_ { nullptr };
    FolderOperationsController    *folderOperations_ { nullptr };
    QHash<QUuid, PendingMarkers>   publicationMarkers_;
    QHash<QString, PendingMarkers> pendingMarkers_;
    QSet<QString>                  pendingFolderOverlayStorageIds_;
    QSet<QString>                  pendingFolderOverlayLoads_;
    bool                           markerFlushScheduled_ { false };
    bool                           folderOverlayImportScheduled_ { false };
    bool                           initialized_ { false };

    DraftStoreError routeDraft(DraftRecord *record);
    void            handleDraftPublished(const QUuid &draftId, const Note &note);
    void            flushMarkers();
    void            enqueueMarkers(const QList<QUuid> &ruleIds, const NoteRuleEvaluationInput &input);
    void            queueFolderOverlayImport(const QString &storageId);
    void            queueAllFolderOverlayImports();
    void            processFolderOverlayImports();
    void            importFolderOverlays(const QString &storageId);
    void            loadAndImportFolderOverlay(const QString &storageId, const QString &noteId);
    void            applyFolderOverlayRules(const Note &note);
    void applyFolderOverlayEvaluation(const NoteRuleEvaluationInput &input, const NoteRuleEvaluation &evaluation);
    bool supportsFolderOverlayImport(const QString &storageId) const;
    void reportFailure(const QString &storageId, const QString &noteId, const QString &message);
    NoteRuleEvaluation             evaluateRules(const NoteRuleEvaluationInput &input) const;
    NoteRuleEvaluation             evaluateFolderOverlayRules(const NoteRuleEvaluationInput &input) const;
    NoteRuleEvaluationInput        evaluationInput(const DraftRecord &record) const;
    static NoteRuleEvaluationInput overlayInput(const Note &note);
    static QString                 markerBatchKey(const NoteRuleEvaluationInput &input);
    static QString                 overlayLoadKey(const QString &storageId, const QString &noteId);
};

} // namespace AnyKeep

#endif // NOTERULEAPPLICATIONCONTROLLER_H
