/*
Copyright (C) 2012  Till Theato <root@ttill.de>
This file is part of kdenlive. See www.kdenlive.org.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "projectmanager.h"
#include "project.h"
#include "core.h"
#include "mainwindow.h"
#include "bin/bin.h"
#include "timelineview/timelinewidget.h"
#include "monitor/monitorview.h"
#include <KFileDialog>
#include <KActionCollection>


ProjectManager::ProjectManager(const KUrl& projectUrl, const QString& clipsToLoad, QObject* parent) :
    QObject(parent),
    m_project(NULL)
{
    KStandardAction::open(this, SLOT(execOpenFileDialog()), pCore->window()->actionCollection());

    if (!projectUrl.isEmpty()) {
        openProject(projectUrl);
    }
}

ProjectManager::~ProjectManager()
{
    if (m_project) {
        delete m_project;
    }
}

Project* ProjectManager::current()
{
    return m_project;
}

void ProjectManager::execOpenFileDialog()
{
    KUrl url = KFileDialog::getOpenUrl(KUrl("kfiledialog:///projectfolder"), "application/x-kdenlive");
    if (!url.isEmpty()) {
        openProject(url);
    }
}

void ProjectManager::openProject(const KUrl& url)
{
    if (m_project) {
        delete m_project;
    }
    m_project = new Project(url, this);
    pCore->window()->bin()->setProject(m_project);
    pCore->window()->timelineWidget()->setProject(m_project);
    pCore->window()->monitorWidget()->setModel(m_project->monitor());
}

#include "projectmanager.moc"
