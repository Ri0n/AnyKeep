#include "settingscontroller.h"

#include <QSettings>

#include <utility>

namespace QtNote {

SettingsController::SettingsController(QObject *parent) : QAbstractListModel(parent) { }

int SettingsController::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : fields_.size(); }

QVariant SettingsController::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= fields_.size())
        return {};
    const auto &field = fields_.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case LabelRole:
        return field.label;
    case KeyRole:
        return field.key;
    case DescriptionRole:
        return field.description;
    case FieldTypeRole:
        return int(field.type);
    case FieldValueRole:
        return field.value;
    case OptionsRole:
        return field.options;
    case MinimumRole:
        return field.minimum;
    case MaximumRole:
        return field.maximum;
    case PlaceholderRole:
        return field.placeholder;
    case RestartRequiredRole:
        return field.restartRequired;
    default:
        return {};
    }
}

bool SettingsController::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != FieldValueRole || !index.isValid() || index.row() < 0 || index.row() >= fields_.size())
        return false;
    auto &field = fields_[index.row()];
    if (field.type == ReadOnly || field.value == value)
        return field.type != ReadOnly;
    field.value = value;
    emit dataChanged(index, index, { FieldValueRole });
    updateDirty();
    return true;
}

Qt::ItemFlags SettingsController::flags(const QModelIndex &index) const
{
    auto result = QAbstractListModel::flags(index);
    if (index.isValid() && fields_.at(index.row()).type != ReadOnly)
        result |= Qt::ItemIsEditable;
    return result;
}

QHash<int, QByteArray> SettingsController::roleNames() const
{
    return {
        { KeyRole, "key" },
        { LabelRole, "label" },
        { DescriptionRole, "description" },
        { FieldTypeRole, "fieldType" },
        { FieldValueRole, "fieldValue" },
        { OptionsRole, "options" },
        { MinimumRole, "minimum" },
        { MaximumRole, "maximum" },
        { PlaceholderRole, "placeholder" },
        { RestartRequiredRole, "restartRequired" },
    };
}

bool    SettingsController::dirty() const { return dirty_; }
QString SettingsController::errorString() const { return errorString_; }

bool SettingsController::setValue(int row, const QVariant &value) { return setData(index(row), value, FieldValueRole); }

QVariant SettingsController::value(const QString &key) const
{
    for (const auto &field : fields_) {
        if (field.key == key)
            return field.value;
    }
    return {};
}

bool SettingsController::apply()
{
    QString error;
    if (!applyValues(values(), &error)) {
        setErrorString(error.isEmpty() ? tr("Failed to apply settings") : error);
        return false;
    }
    originalValues_ = values();
    setErrorString({});
    updateDirty();
    emit applied();
    return true;
}

void SettingsController::reset()
{
    for (int row = 0; row < fields_.size(); ++row) {
        auto      &field    = fields_[row];
        const auto restored = originalValues_.value(field.key);
        if (field.value != restored) {
            field.value = restored;
            emit dataChanged(index(row), index(row), { FieldValueRole });
        }
    }
    setErrorString({});
    updateDirty();
    afterReset();
}

void SettingsController::addField(Field field)
{
    const int row = fields_.size();
    beginInsertRows({}, row, row);
    originalValues_.insert(field.key, field.value);
    fields_.append(std::move(field));
    endInsertRows();
}

void SettingsController::setFields(QList<Field> fields)
{
    beginResetModel();
    fields_ = std::move(fields);
    originalValues_.clear();
    for (const auto &field : std::as_const(fields_))
        originalValues_.insert(field.key, field.value);
    dirty_ = false;
    endResetModel();
    emit dirtyChanged();
}

void SettingsController::setErrorString(const QString &error)
{
    if (errorString_ == error)
        return;
    errorString_ = error;
    emit errorStringChanged();
}

void SettingsController::setDirty(bool dirty)
{
    if (dirty_ == dirty)
        return;
    dirty_ = dirty;
    emit dirtyChanged();
}

QVariantMap SettingsController::values() const
{
    QVariantMap result;
    for (const auto &field : fields_)
        result.insert(field.key, field.value);
    return result;
}

void SettingsController::updateDirty() { setDirty(values() != originalValues_); }

PersistentSettingsController::PersistentSettingsController(QString settingsGroup, QList<Field> fields,
                                                           QObject *parent) :
    SettingsController(parent), settingsGroup_(std::move(settingsGroup))
{
    QSettings settings;
    settings.beginGroup(settingsGroup_);
    for (auto &field : fields)
        field.value = settings.value(field.key, field.value);
    setFields(std::move(fields));
}

bool PersistentSettingsController::applyValues(const QVariantMap &values, QString *)
{
    QSettings settings;
    settings.beginGroup(settingsGroup_);
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        settings.setValue(it.key(), it.value());
    return true;
}

} // namespace QtNote
