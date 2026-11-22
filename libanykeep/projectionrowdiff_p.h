#ifndef PROJECTIONROWDIFF_P_H
#define PROJECTIONROWDIFF_P_H

#include <QHash>
#include <QList>
#include <QSet>

#include <algorithm>
#include <utility>

namespace AnyKeep {

// Keys are unique within each projection. Callbacks mutate the attached model
// with the matching Qt notifications. Move destinations are final row numbers.
template <typename Key, typename Remove, typename Move, typename Insert>
void reconcileProjectionRows(QList<Key> current, const QList<Key> &next, Remove remove, Move move, Insert insert)
{
    if (current == next)
        return;

    QHash<Key, int> nextRows;
    nextRows.reserve(next.size());
    for (int row = 0; row < next.size(); ++row)
        nextRows.insert(next.at(row), row);

    for (int last = current.size() - 1; last >= 0;) {
        if (nextRows.contains(current.at(last))) {
            --last;
            continue;
        }
        int first = last;
        while (first > 0 && !nextRows.contains(current.at(first - 1)))
            --first;
        remove(first, last);
        current.remove(first, last - first + 1);
        last = first - 1;
    }

    const QSet<Key> retained(current.cbegin(), current.cend());
    QList<Key>      retainedOrder;
    retainedOrder.reserve(current.size());
    for (const auto &key : next)
        if (retained.contains(key))
            retainedOrder.append(key);

    if (current != retainedOrder) {
        // Keep the longest subsequence already in the right relative order.
        // Moving the first row to the end should move just that row.
        QList<int> ranks;
        ranks.reserve(current.size());
        for (const auto &key : std::as_const(current))
            ranks.append(nextRows.value(key));
        QList<int> predecessors(current.size(), -1);
        QList<int> tails;
        for (int row = 0; row < ranks.size(); ++row) {
            auto tail = std::lower_bound(tails.begin(), tails.end(), ranks.at(row),
                                         [&ranks](int candidate, int rank) { return ranks.at(candidate) < rank; });
            if (tail != tails.begin())
                predecessors[row] = *(tail - 1);
            if (tail == tails.end())
                tails.append(row);
            else
                *tail = row;
        }
        QSet<Key> stationary;
        for (int row = tails.constLast(); row >= 0; row = predecessors.at(row))
            stationary.insert(current.at(row));

        for (int row = retainedOrder.size() - 1; row >= 0; --row) {
            const auto &key = retainedOrder.at(row);
            if (stationary.contains(key))
                continue;
            const int from = current.indexOf(key);
            const int before
                = row + 1 == retainedOrder.size() ? current.size() : current.indexOf(retainedOrder.at(row + 1));
            const int to = from < before ? before - 1 : before;
            if (from != to) {
                move(from, to);
                current.move(from, to);
            }
        }
    }

    for (int first = 0; first < next.size();) {
        if (retained.contains(next.at(first))) {
            ++first;
            continue;
        }
        int last = first;
        while (last + 1 < next.size() && !retained.contains(next.at(last + 1)))
            ++last;
        insert(first, last);
        first = last + 1;
    }
}

} // namespace AnyKeep

#endif // PROJECTIONROWDIFF_P_H
