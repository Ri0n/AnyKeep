#ifndef RULESCONTROLLER_H
#define RULESCONTROLLER_H

#include "noterule.h"
#include "qtnote_export.h"

#include <QAbstractListModel>
#include <QVariantList>

namespace QtNote {

class FolderCatalogManager;
class NoteManager;
class NoteRuleManager;

/** QML-facing model/editor for the encrypted local note-routing rules. */
class QTNOTE_EXPORT RulesController final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(QVariantList folderChoices READ folderChoices NOTIFY choicesChanged)
    Q_PROPERTY(QVariantList storageChoices READ storageChoices NOTIFY choicesChanged)

public:
    enum Role {
        RuleIdRole = Qt::UserRole + 1,
        NameRole,
        EnabledRole,
        SummaryRole,
        StopProcessingRole,
    };
    Q_ENUM(Role)

    explicit RulesController(NoteRuleManager      *ruleManager          = nullptr,
                             FolderCatalogManager *folderCatalogManager = nullptr, NoteManager *noteManager = nullptr,
                             QObject *parent = nullptr);

    int                    rowCount(const QModelIndex &parent = {}) const override;
    QVariant               data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool         available() const;
    QString      errorString() const { return errorString_; }
    QVariantList folderChoices() const;
    QVariantList storageChoices() const;

    Q_INVOKABLE QString createRule();
    Q_INVOKABLE bool    removeRule(const QString &ruleId);
    /** Moves a row to its final destination row, not a Qt insertion offset. */
    Q_INVOKABLE bool        moveRule(int sourceRow, int destinationRow);
    Q_INVOKABLE bool        setRuleEnabled(const QString &ruleId, bool enabled);
    Q_INVOKABLE QVariantMap ruleDetails(const QString &ruleId) const;
    Q_INVOKABLE bool        updateRule(const QString &ruleId, const QString &name, int conditionCombiner,
                                       const QVariantList &conditions, const QVariantList &actions, bool stopProcessing);
    Q_INVOKABLE QString     conditionLabel(int kind) const;
    Q_INVOKABLE QString     actionLabel(int kind) const;

signals:
    void availableChanged();
    void errorStringChanged();
    void choicesChanged();

private:
    NoteRuleManager      *ruleManager_ { nullptr };
    FolderCatalogManager *folderCatalogManager_ { nullptr };
    NoteManager          *noteManager_ { nullptr };
    QString               errorString_;

    QList<NoteRule>    orderedRules() const;
    void               resetModel();
    void               setError(const QString &message);
    bool               checkFolderTarget(const QUuid &folderId);
    static QVariantMap conditionMap(const NoteRuleCondition &condition);
    static QVariantMap actionMap(const NoteRuleAction &action);
    static bool        conditionFromMap(const QVariant &value, NoteRuleCondition *condition, QString *error);
    static bool        actionFromMap(const QVariant &value, NoteRuleAction *action, QString *error);
};

} // namespace QtNote

#endif // RULESCONTROLLER_H
