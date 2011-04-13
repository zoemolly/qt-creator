/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2011 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (info@qt.nokia.com)
**
**
** GNU Lesser General Public License Usage
**
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this file.
** Please review the following information to ensure the GNU Lesser General
** Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** Other Usage
**
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
** If you have questions regarding the use of this file, please contact
** Nokia at qt-info@nokia.com.
**
**************************************************************************/

#include "buildmanager.h"

#include "buildprogress.h"
#include "buildsteplist.h"
#include "compileoutputwindow.h"
#include "projectexplorerconstants.h"
#include "projectexplorer.h"
#include "project.h"
#include "projectexplorersettings.h"
#include "target.h"
#include "taskwindow.h"
#include "taskhub.h"
#include "buildconfiguration.h"

#include <coreplugin/icore.h>
#include <coreplugin/progressmanager/progressmanager.h>
#include <coreplugin/progressmanager/futureprogress.h>
#include <projectexplorer/session.h>
#include <extensionsystem/pluginmanager.h>
#include <utils/qtcassert.h>

#include <QtCore/QDir>
#include <QtCore/QTimer>
#include <QtCore/QMetaType>
#include <QtCore/QList>
#include <QtCore/QHash>
#include <QtCore/QFutureWatcher>

#include <qtconcurrent/QtConcurrentTools>

#include <QtGui/QApplication>
#include <QtGui/QMainWindow>

static inline QString msgProgress(int progress, int total)
{
    return ProjectExplorer::BuildManager::tr("Finished %1 of %n build steps", 0, total).arg(progress);
}

namespace ProjectExplorer {
//NBS TODO this class has too many different variables which hold state:
// m_buildQueue, m_running, m_canceled, m_progress, m_maxProgress, m_activeBuildSteps and ...
// I might need to reduce that.
struct BuildManagerPrivate {
    BuildManagerPrivate();

    Internal::CompileOutputWindow *m_outputWindow;
    TaskHub *m_taskHub;
    Internal::TaskWindow *m_taskWindow;

    QList<BuildStep *> m_buildQueue;
    QStringList m_configurations; // the corresponding configuration to the m_buildQueue
    ProjectExplorerPlugin *m_projectExplorerPlugin;
    bool m_running;
    QFutureWatcher<bool> m_watcher;
    BuildStep *m_currentBuildStep;
    QString m_currentConfiguration;
    // used to decide if we are building a project to decide when to emit buildStateChanged(Project *)
    QHash<Project *, int>  m_activeBuildSteps;
    Project *m_previousBuildStepProject;
    // is set to true while canceling, so that nextBuildStep knows that the BuildStep finished because of canceling
    bool m_canceling;

    // Progress reporting to the progress manager
    int m_progress;
    int m_maxProgress;
    QFutureInterface<void> *m_progressFutureInterface;
    QFutureWatcher<void> m_progressWatcher;
};

BuildManagerPrivate::BuildManagerPrivate() :
    m_running(false)
  , m_previousBuildStepProject(0)
  , m_canceling(false)
  , m_maxProgress(0)
  , m_progressFutureInterface(0)
{
}

BuildManager::BuildManager(ProjectExplorerPlugin *parent)
    : QObject(parent), d(new BuildManagerPrivate)
{
    ExtensionSystem::PluginManager *pm = ExtensionSystem::PluginManager::instance();
    d->m_projectExplorerPlugin = parent;

    connect(&d->m_watcher, SIGNAL(finished()),
            this, SLOT(nextBuildQueue()));

    connect(&d->m_watcher, SIGNAL(progressValueChanged(int)),
            this, SLOT(progressChanged()));
    connect(&d->m_watcher, SIGNAL(progressTextChanged(QString)),
            this, SLOT(progressTextChanged()));
    connect(&d->m_watcher, SIGNAL(progressRangeChanged(int, int)),
            this, SLOT(progressChanged()));

    connect(parent->session(), SIGNAL(aboutToRemoveProject(ProjectExplorer::Project *)),
            this, SLOT(aboutToRemoveProject(ProjectExplorer::Project *)));

    d->m_outputWindow = new Internal::CompileOutputWindow(this);
    pm->addObject(d->m_outputWindow);

    d->m_taskHub = pm->getObject<TaskHub>();
    d->m_taskWindow = new Internal::TaskWindow(d->m_taskHub);
    pm->addObject(d->m_taskWindow);

    qRegisterMetaType<ProjectExplorer::BuildStep::OutputFormat>();

    connect(d->m_taskWindow, SIGNAL(tasksChanged()),
            this, SLOT(updateTaskCount()));

    connect(d->m_taskWindow, SIGNAL(tasksCleared()),
            this,SIGNAL(tasksCleared()));

    connect(&d->m_progressWatcher, SIGNAL(canceled()),
            this, SLOT(cancel()));
    connect(&d->m_progressWatcher, SIGNAL(finished()),
            this, SLOT(finish()));
}

void BuildManager::extensionsInitialized()
{
    d->m_taskHub->addCategory(Constants::TASK_CATEGORY_COMPILE, tr("Compile", "Category for compiler isses listened under 'Build Issues'"));
    d->m_taskHub->addCategory(Constants::TASK_CATEGORY_BUILDSYSTEM, tr("Build System", "Category for build system isses listened under 'Build Issues'"));
}

BuildManager::~BuildManager()
{
    cancel();
    ExtensionSystem::PluginManager *pm = ExtensionSystem::PluginManager::instance();

    pm->removeObject(d->m_taskWindow);
    delete d->m_taskWindow;

    pm->removeObject(d->m_outputWindow);
    delete d->m_outputWindow;
}

void BuildManager::aboutToRemoveProject(ProjectExplorer::Project *p)
{
    QHash<Project *, int>::iterator it = d->m_activeBuildSteps.find(p);
    QHash<Project *, int>::iterator end = d->m_activeBuildSteps.end();
    if (it != end && *it > 0) {
        // We are building the project that's about to be removed.
        // We cancel the whole queue, which isn't the nicest thing to do
        // but a safe thing.
        cancel();
    }
}

bool BuildManager::isBuilding() const
{
    // we are building even if we are not running yet
    return !d->m_buildQueue.isEmpty() || d->m_running;
}

void BuildManager::cancel()
{
    if (d->m_running) {
        d->m_canceling = true;
        d->m_watcher.cancel();
        d->m_watcher.waitForFinished();

        // The cancel message is added to the output window via a single shot timer
        // since the canceling is likely to have generated new addToOutputWindow signals
        // which are waiting in the event queue to be processed
        // (And we want those to be before the cancel message.)
        QTimer::singleShot(0, this, SLOT(emitCancelMessage()));

        disconnect(d->m_currentBuildStep, SIGNAL(addTask(ProjectExplorer::Task)),
                   this, SLOT(addToTaskWindow(ProjectExplorer::Task)));
        disconnect(d->m_currentBuildStep, SIGNAL(addOutput(QString, ProjectExplorer::BuildStep::OutputFormat)),
                   this, SLOT(addToOutputWindow(QString, ProjectExplorer::BuildStep::OutputFormat)));
        decrementActiveBuildSteps(d->m_currentBuildStep->buildConfiguration()->target()->project());

        d->m_progressFutureInterface->setProgressValueAndText(d->m_progress*100, tr("Build canceled")); //TODO NBS fix in qtconcurrent
        clearBuildQueue();
    }
    return;
}

void BuildManager::updateTaskCount()
{
    Core::ProgressManager *progressManager = Core::ICore::instance()->progressManager();
    const int errors = d->m_taskWindow->errorTaskCount();
    if (errors > 0) {
        progressManager->setApplicationLabel(QString::number(errors));
    } else {
        progressManager->setApplicationLabel(QString());
    }
    emit tasksChanged();
}

void BuildManager::finish()
{
    QApplication::alert(Core::ICore::instance()->mainWindow(), 3000);
}

void BuildManager::emitCancelMessage()
{
    emit addToOutputWindow(tr("Canceled build."), BuildStep::ErrorMessageOutput);
}

void BuildManager::clearBuildQueue()
{
    foreach (BuildStep *bs, d->m_buildQueue) {
        decrementActiveBuildSteps(bs->buildConfiguration()->target()->project());
        disconnect(bs, SIGNAL(addTask(ProjectExplorer::Task)),
                   this, SLOT(addToTaskWindow(ProjectExplorer::Task)));
        disconnect(bs, SIGNAL(addOutput(QString, ProjectExplorer::BuildStep::OutputFormat)),
                   this, SLOT(addToOutputWindow(QString, ProjectExplorer::BuildStep::OutputFormat)));
    }

    d->m_buildQueue.clear();
    d->m_running = false;
    d->m_previousBuildStepProject = 0;
    d->m_currentBuildStep = 0;

    d->m_progressFutureInterface->reportCanceled();
    d->m_progressFutureInterface->reportFinished();
    d->m_progressWatcher.setFuture(QFuture<void>());
    delete d->m_progressFutureInterface;
    d->m_progressFutureInterface = 0;
    d->m_maxProgress = 0;

    emit buildQueueFinished(false);
}


void BuildManager::toggleOutputWindow()
{
    d->m_outputWindow->toggle(false);
}

void BuildManager::showTaskWindow()
{
    d->m_taskWindow->popup(false);
}

void BuildManager::toggleTaskWindow()
{
    d->m_taskWindow->toggle(false);
}

bool BuildManager::tasksAvailable() const
{
    return d->m_taskWindow->taskCount() > 0;
}

void BuildManager::startBuildQueue()
{
    if (d->m_buildQueue.isEmpty()) {
        emit buildQueueFinished(true);
        return;
    }
    if (!d->m_running) {
        // Progress Reporting
        Core::ProgressManager *progressManager = Core::ICore::instance()->progressManager();
        d->m_progressFutureInterface = new QFutureInterface<void>;
        d->m_progressWatcher.setFuture(d->m_progressFutureInterface->future());
        d->m_outputWindow->clearContents();
        d->m_taskHub->clearTasks(Constants::TASK_CATEGORY_COMPILE);
        d->m_taskHub->clearTasks(Constants::TASK_CATEGORY_BUILDSYSTEM);
        progressManager->setApplicationLabel(QString());
        Core::FutureProgress *progress = progressManager->addTask(d->m_progressFutureInterface->future(),
              tr("Build"),
              Constants::TASK_BUILD,
              Core::ProgressManager::KeepOnFinish | Core::ProgressManager::ShowInApplicationIcon);
        connect(progress, SIGNAL(clicked()), this, SLOT(showBuildResults()));
        progress->setWidget(new Internal::BuildProgress(d->m_taskWindow));
        d->m_progress = 0;
        d->m_progressFutureInterface->setProgressRange(0, d->m_maxProgress * 100);

        d->m_running = true;
        d->m_canceling = false;
        d->m_progressFutureInterface->reportStarted();
        nextStep();
    } else {
        // Already running
        d->m_progressFutureInterface->setProgressRange(0, d->m_maxProgress * 100);
        d->m_progressFutureInterface->setProgressValueAndText(d->m_progress*100, msgProgress(d->m_progress, d->m_maxProgress));
    }
}

void BuildManager::showBuildResults()
{
    if (d->m_taskWindow->taskCount() != 0)
        toggleTaskWindow();
    else
        toggleOutputWindow();
    //toggleTaskWindow();
}

void BuildManager::addToTaskWindow(const ProjectExplorer::Task &task)
{
    d->m_outputWindow->registerPositionOf(task);
    // Distribute to all others
    d->m_taskHub->addTask(task);
}

void BuildManager::addToOutputWindow(const QString &string, ProjectExplorer::BuildStep::OutputFormat format)
{
    d->m_outputWindow->appendText(string, format);
}

void BuildManager::nextBuildQueue()
{
    if (d->m_canceling)
        return;

    disconnect(d->m_currentBuildStep, SIGNAL(addTask(ProjectExplorer::Task)),
               this, SLOT(addToTaskWindow(ProjectExplorer::Task)));
    disconnect(d->m_currentBuildStep, SIGNAL(addOutput(QString, ProjectExplorer::BuildStep::OutputFormat)),
               this, SLOT(addToOutputWindow(QString, ProjectExplorer::BuildStep::OutputFormat)));

    ++d->m_progress;
    d->m_progressFutureInterface->setProgressValueAndText(d->m_progress*100, msgProgress(d->m_progress, d->m_maxProgress));
    decrementActiveBuildSteps(d->m_currentBuildStep->buildConfiguration()->target()->project());

    bool result = d->m_watcher.result();
    if (!result) {
        // Build Failure
        const QString projectName = d->m_currentBuildStep->buildConfiguration()->target()->project()->displayName();
        const QString targetName = d->m_currentBuildStep->buildConfiguration()->target()->displayName();
        addToOutputWindow(tr("Error while building project %1 (target: %2)").arg(projectName, targetName), BuildStep::ErrorOutput);
        addToOutputWindow(tr("When executing build step '%1'").arg(d->m_currentBuildStep->displayName()), BuildStep::ErrorOutput);
        // NBS TODO fix in qtconcurrent
        d->m_progressFutureInterface->setProgressValueAndText(d->m_progress*100, tr("Error while building project %1 (target: %2)").arg(projectName, targetName));
    }

    if (result)
        nextStep();
    else
        clearBuildQueue();
}

void BuildManager::progressChanged()
{
    if (!d->m_progressFutureInterface)
        return;
    int range = d->m_watcher.progressMaximum() - d->m_watcher.progressMinimum();
    if (range != 0) {
        int percent = (d->m_watcher.progressValue() - d->m_watcher.progressMinimum()) * 100 / range;
        d->m_progressFutureInterface->setProgressValueAndText(d->m_progress * 100 + percent, msgProgress(d->m_progress, d->m_maxProgress) + "\n" + d->m_watcher.progressText());
    }
}

void BuildManager::progressTextChanged()
{
    int range = d->m_watcher.progressMaximum() - d->m_watcher.progressMinimum();
    int percent = 0;
    if (range != 0)
        percent = (d->m_watcher.progressValue() - d->m_watcher.progressMinimum()) * 100 / range;
    d->m_progressFutureInterface->setProgressValueAndText(d->m_progress*100 + percent, msgProgress(d->m_progress, d->m_maxProgress) + "\n" + d->m_watcher.progressText());
}

void BuildManager::nextStep()
{
    if (!d->m_buildQueue.empty()) {
        d->m_currentBuildStep = d->m_buildQueue.front();
        d->m_buildQueue.pop_front();

        if (d->m_currentBuildStep->buildConfiguration()->target()->project() != d->m_previousBuildStepProject) {
            const QString projectName = d->m_currentBuildStep->buildConfiguration()->target()->project()->displayName();
            addToOutputWindow(tr("Running build steps for project %1...")
                              .arg(projectName), BuildStep::MessageOutput);
            d->m_previousBuildStepProject = d->m_currentBuildStep->buildConfiguration()->target()->project();
        }
        d->m_watcher.setFuture(QtConcurrent::run(&BuildStep::run, d->m_currentBuildStep));
    } else {
        d->m_running = false;
        d->m_previousBuildStepProject = 0;
        d->m_progressFutureInterface->reportFinished();
        d->m_progressWatcher.setFuture(QFuture<void>());
        d->m_currentBuildStep = 0;
        delete d->m_progressFutureInterface;
        d->m_progressFutureInterface = 0;
        d->m_maxProgress = 0;
        emit buildQueueFinished(true);
    }
}

bool BuildManager::buildQueueAppend(QList<BuildStep *> steps)
{
    int count = steps.size();
    bool init = true;
    int i = 0;
    for (; i < count; ++i) {
        BuildStep *bs = steps.at(i);
        connect(bs, SIGNAL(addTask(ProjectExplorer::Task)),
                this, SLOT(addToTaskWindow(ProjectExplorer::Task)));
        connect(bs, SIGNAL(addOutput(QString, ProjectExplorer::BuildStep::OutputFormat)),
                this, SLOT(addToOutputWindow(QString, ProjectExplorer::BuildStep::OutputFormat)));
        init = bs->init();
        if (!init)
            break;
    }
    if (!init) {
        BuildStep *bs = steps.at(i);

        // cleaning up
        // print something for the user
        const QString projectName = bs->buildConfiguration()->target()->project()->displayName();
        const QString targetName = bs->buildConfiguration()->target()->displayName();
        addToOutputWindow(tr("Error while building project %1 (target: %2)").arg(projectName, targetName), BuildStep::ErrorOutput);
        addToOutputWindow(tr("When executing build step '%1'").arg(bs->displayName()), BuildStep::ErrorOutput);

        // disconnect the buildsteps again
        for (int j = 0; j <= i; ++j) {
            BuildStep *bs = steps.at(j);
            disconnect(bs, SIGNAL(addTask(ProjectExplorer::Task)),
                       this, SLOT(addToTaskWindow(ProjectExplorer::Task)));
            disconnect(bs, SIGNAL(addOutput(QString, ProjectExplorer::BuildStep::OutputFormat)),
                       this, SLOT(addToOutputWindow(QString, ProjectExplorer::BuildStep::OutputFormat)));
        }
        return false;
    }

    // Everthing init() well
    for (i = 0; i < count; ++i) {
        ++d->m_maxProgress;
        d->m_buildQueue.append(steps.at(i));
        incrementActiveBuildSteps(steps.at(i)->buildConfiguration()->target()->project());
    }
    return true;
}

bool BuildManager::buildList(BuildStepList *bsl)
{
    return buildLists(QList<BuildStepList *>() << bsl);
}

bool BuildManager::buildLists(QList<BuildStepList *> bsls)
{
    QList<BuildStep *> steps;
    foreach(BuildStepList *list, bsls)
        steps.append(list->steps());

    bool success = buildQueueAppend(steps);
    if (!success) {
        d->m_outputWindow->popup(false);
        return false;
    }

    if (ProjectExplorerPlugin::instance()->projectExplorerSettings().showCompilerOutput)
        d->m_outputWindow->popup(false);
    startBuildQueue();
    return true;
}

void BuildManager::appendStep(BuildStep *step)
{
    bool success = buildQueueAppend(QList<BuildStep *>() << step);
    if (!success) {
        d->m_outputWindow->popup(false);
        return;
    }
    if (ProjectExplorerPlugin::instance()->projectExplorerSettings().showCompilerOutput)
        d->m_outputWindow->popup(false);
    startBuildQueue();
}

bool BuildManager::isBuilding(Project *pro)
{
    QHash<Project *, int>::iterator it = d->m_activeBuildSteps.find(pro);
    QHash<Project *, int>::iterator end = d->m_activeBuildSteps.end();
    if (it == end || *it == 0)
        return false;
    else
        return true;
}

bool BuildManager::isBuilding(BuildStep *step)
{
    return (d->m_currentBuildStep == step) || d->m_buildQueue.contains(step);
}

void BuildManager::incrementActiveBuildSteps(Project *pro)
{
    QHash<Project *, int>::iterator it = d->m_activeBuildSteps.find(pro);
    QHash<Project *, int>::iterator end = d->m_activeBuildSteps.end();
    if (it == end) {
        d->m_activeBuildSteps.insert(pro, 1);
        emit buildStateChanged(pro);
    } else if (*it == 0) {
        ++*it;
        emit buildStateChanged(pro);
    } else {
        ++*it;
    }
}

void BuildManager::decrementActiveBuildSteps(Project *pro)
{
    QHash<Project *, int>::iterator it = d->m_activeBuildSteps.find(pro);
    QHash<Project *, int>::iterator end = d->m_activeBuildSteps.end();
    if (it == end) {
        Q_ASSERT(false && "BuildManager d->m_activeBuildSteps says project is not building, but apparently a build step was still in the queue.");
    } else if (*it == 1) {
        --*it;
        emit buildStateChanged(pro);
    } else {
        --*it;
    }
}

} // namespace ProjectExplorer
