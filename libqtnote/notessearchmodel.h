#ifndef NOTESSEARCHMODEL_H
#define NOTESSEARCHMODEL_H

#include "qtnote_export.h"

#include <QHash>
#include <QPointer>
#include <QSortFilterProxyModel>
#include <QStringList>

namespace QtNote {

class GlobalNoteFinder;

class QTNOTE_EXPORT NotesSearchModel final : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(bool searchInBody READ searchInBody WRITE setSearchInBody NOTIFY searchInBodyChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)

public:
    explicit NotesSearchModel(QObject *parent = nullptr);

    QString searchText() const { return text_; }
    bool    searchInBody() const { return searchInBody_; }
    bool    searching() const { return searching_; }
    bool    hasBodyMatch(const QString &storageId, const QString &noteId) const;

public slots:
    void setSearchText(const QString &text);
    void setSearchInBody(bool allow);

signals:
    void searchTextChanged();
    void searchInBodyChanged();
    void searchingChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private slots:
    void noteFound(const QString &storageId, const QString &noteId);
    void searchCompleted();

private:
    void restartBodySearch();
    void setSearching(bool searching);
    void invalidateRowsFilter();

    bool                        searchInBody_ { false };
    bool                        searching_ { false };
    QString                     text_;
    QPointer<GlobalNoteFinder>  finder_;
    QHash<QString, QStringList> foundCache_;
};

} // namespace QtNote

#endif // NOTESSEARCHMODEL_H
