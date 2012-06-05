/*
Copyright (C) 2012  Till Theato <root@ttill.de>
This file is part of kdenlive. See www.kdenlive.org.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef CORE_H
#define CORE_H

#include <QObject>

class MainWindow;
class ProjectManager;
class ClipPluginManager;
class EffectRepository;
class Project;
class PluginManager;
class KUrl;


#define pCore Core::self()


class Core : public QObject
{
    Q_OBJECT

public:
    virtual ~Core();

    static void initialize(MainWindow *mainWindow, const KUrl &projectUrl, const QString &clipsToLoad);

    static Core *self();

    MainWindow *window();
    ProjectManager *projectManager();
    EffectRepository *effectRepository();
    ClipPluginManager *clipPluginManager();
//     PluginManager *pluginManager();

private:
    Core(MainWindow *mainWindow);
    static Core *m_self;
    void init(const KUrl &projectUrl, const QString &clipsToLoad);

    MainWindow *m_mainWindow;
    Project *m_currentProject;
    EffectRepository *m_effectRepository;
    ClipPluginManager *m_clipPluginManager;
    ProjectManager *m_projectManager;
//     PluginManager *m_pluginManager;
};

#endif
