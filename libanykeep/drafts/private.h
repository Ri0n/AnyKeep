#ifndef ANYKEEP_DRAFTS_PRIVATE_H
#define ANYKEEP_DRAFTS_PRIVATE_H

#include "draftstore.h"

#include <QLoggingCategory>
#include <QString>
#include <QVariantMap>

namespace AnyKeep {

Q_DECLARE_LOGGING_CATEGORY(logDraftPersistence)

namespace DraftManagerPrivate {

    const char *draftStateName(DraftRecord::State state);
    const char *draftOperationName(DraftRecord::Operation operation);
    QString     concurrencySummary(const QVariantMap &data);

} // namespace DraftManagerPrivate
} // namespace AnyKeep

#endif // ANYKEEP_DRAFTS_PRIVATE_H
