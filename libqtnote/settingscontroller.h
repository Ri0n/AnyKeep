#ifndef QTNOTE_SETTINGSCONTROLLER_H
#define QTNOTE_SETTINGSCONTROLLER_H

#include "qtnote_export.h"

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QVariant>
#include <QVariantMap>

namespace QtNote {

class QTNOTE_EXPORT SettingsController : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)

public:
    enum FieldType { Text, Password, Multiline, Boolean, Integer, Choice, ReadOnly };
    Q_ENUM(FieldType)

    enum Role {
        KeyRole = Qt::UserRole + 1,
        LabelRole,
        DescriptionRole,
        FieldTypeRole,
        FieldValueRole,
        OptionsRole,
        MinimumRole,
        MaximumRole,
        PlaceholderRole,
        RestartRequiredRole,
    };
    Q_ENUM(Role)

    struct Field {
        QString      key;
        QString      label;
        QString      description;
        FieldType    type { Text };
        QVariant     value;
        QVariantList options;
        int          minimum { 0 };
        int          maximum { 100 };
        QString      placeholder;
        bool         restartRequired { false };
    };

    explicit SettingsController(QObject *parent = nullptr);

    int                    rowCount(const QModelIndex &parent = {}) const override;
    QVariant               data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool                   setData(const QModelIndex &index, const QVariant &value, int role) override;
    Qt::ItemFlags          flags(const QModelIndex &index) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool    dirty() const;
    QString errorString() const;

    Q_INVOKABLE bool     setValue(int row, const QVariant &value);
    Q_INVOKABLE QVariant value(const QString &key) const;
    Q_INVOKABLE bool     apply();
    Q_INVOKABLE void     reset();

signals:
    void dirtyChanged();
    void errorStringChanged();
    void applied();

protected:
    void         addField(Field field);
    void         setFields(QList<Field> fields);
    void         setErrorString(const QString &error);
    void         setDirty(bool dirty);
    virtual bool applyValues(const QVariantMap &values, QString *error) = 0;
    virtual void afterReset() { }

private:
    QVariantMap values() const;
    void        updateDirty();

    QList<Field> fields_;
    QVariantMap  originalValues_;
    QString      errorString_;
    bool         dirty_ { false };
};

class QTNOTE_EXPORT PersistentSettingsController final : public SettingsController {
    Q_OBJECT

public:
    explicit PersistentSettingsController(QString settingsGroup, QList<Field> fields, QObject *parent = nullptr);

protected:
    bool applyValues(const QVariantMap &values, QString *error) override;

private:
    QString settingsGroup_;
};

} // namespace QtNote

#endif // QTNOTE_SETTINGSCONTROLLER_H
