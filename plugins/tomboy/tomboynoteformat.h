/*
QtNote - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef TOMBOYNOTEFORMAT_H
#define TOMBOYNOTEFORMAT_H

#include <QDomDocument>
#include <QDomElement>
#include <QString>

namespace QtNote::TomboyNoteFormat {

QString markdownFromContent(const QDomElement &content, const QString &title);
void    appendMarkdownContent(QDomDocument &dom, QDomElement &content, const QString &title, const QString &markdown);

} // namespace QtNote::TomboyNoteFormat

#endif // TOMBOYNOTEFORMAT_H
