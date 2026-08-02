#pragma once

#include "draftstore.h"
#include "notedata.h"

namespace QtNote::TestSupport {

class MemoryDraftStore final : public DraftStore {
public:
    DraftStoreError write(const DraftRecord &record) override
    {
        records_.insert(record.id, record);
        return {};
    }

    DraftStoreResult<DraftRecord> load(const QUuid &id) const override
    {
        const auto it = records_.constFind(id);
        if (it == records_.cend())
            return { {}, { DraftStoreError::NotFound, QStringLiteral("not found") } };
        return { it.value(), {} };
    }

    DraftStoreResult<QList<DraftRecord>> records() const override { return { records_.values(), {} }; }

    DraftStoreError transition(const QUuid &id, DraftRecord::State state) override
    {
        auto it = records_.find(id);
        if (it == records_.end())
            return { DraftStoreError::NotFound, QStringLiteral("not found") };
        it->state = state;
        return {};
    }

    DraftStoreError remove(const QUuid &id) override
    {
        return records_.remove(id) ? DraftStoreError {}
                                   : DraftStoreError { DraftStoreError::NotFound, QStringLiteral("not found") };
    }

private:
    QHash<QUuid, DraftRecord> records_;
};

inline Note plainNote()
{
    Note note(new NoteData(nullptr));
    note.setTitle(QStringLiteral("Title"));
    note.setText(QStringLiteral("Body"), Note::PlainText);
    return note;
}

} // namespace QtNote::TestSupport
