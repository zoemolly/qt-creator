/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#pragma once

#include "symbolindexinginterface.h"

#include "symbolindexer.h"
#include "symbolscollector.h"
#include "processormanager.h"
#include "symbolindexertaskqueue.h"
#include "taskscheduler.h"
#include "symbolstorage.h"

#include <usedmacroandsourcestorage.h>

#include <refactoringdatabaseinitializer.h>
#include <filepathcachingfwd.h>

#include <sqlitedatabase.h>
#include <sqlitereadstatement.h>
#include <sqlitewritestatement.h>

#include <QFileSystemWatcher>

#include <thread>

namespace ClangBackEnd {

class SymbolsCollectorManager;

class SymbolsCollectorManager final : public ClangBackEnd::ProcessorManager<SymbolsCollector>
{
public:
    using Processor = SymbolsCollector;
    SymbolsCollectorManager(const ClangBackEnd::GeneratedFiles &generatedFiles,
                            Sqlite::Database &database)
        : ProcessorManager(generatedFiles),
          m_database(database)
    {}

protected:
    std::unique_ptr<SymbolsCollector> createProcessor() const
    {
        return  std::make_unique<SymbolsCollector>(m_database);
    }

private:
    Sqlite::Database &m_database;
};

class SymbolIndexing final : public SymbolIndexingInterface
{
public:
    using UsedMacroAndSourceStorage = ClangBackEnd::UsedMacroAndSourceStorage<Sqlite::Database>;
    using SymbolStorage = ClangBackEnd::SymbolStorage<Sqlite::Database>;
    SymbolIndexing(Sqlite::Database &database,
                   FilePathCachingInterface &filePathCache,
                   const GeneratedFiles &generatedFiles)
        : m_filePathCache(filePathCache),
          m_usedMacroAndSourceStorage(database),
          m_symbolStorage(database),
          m_collectorManger(generatedFiles, database),
          m_indexerScheduler(m_collectorManger, m_indexerQueue, std::thread::hardware_concurrency())
    {
    }

    ~SymbolIndexing()
    {
        syncTasks();
    }

    SymbolIndexer &indexer()
    {
        return m_indexer;
    }

    void syncTasks()
    {
        m_indexerScheduler.disable();
        while (!m_indexerScheduler.futures().empty()) {
            m_indexerScheduler.syncTasks();
            m_indexerScheduler.freeSlots();
        }
    }

    void updateProjectParts(V2::ProjectPartContainers &&projectParts) override;

private:
    using SymbolIndexerTaskScheduler = TaskScheduler<SymbolsCollectorManager, SymbolIndexerTask::Callable>;
    FilePathCachingInterface &m_filePathCache;
    UsedMacroAndSourceStorage m_usedMacroAndSourceStorage;
    SymbolStorage m_symbolStorage;
    ClangPathWatcher<QFileSystemWatcher, QTimer> m_sourceWatcher{m_filePathCache};
    FileStatusCache m_fileStatusCache{m_filePathCache};
    SymbolsCollectorManager m_collectorManger;
    SymbolIndexerTaskScheduler m_indexerScheduler;
    SymbolIndexerTaskQueue m_indexerQueue{m_indexerScheduler};
    SymbolIndexer m_indexer{m_indexerQueue,
                            m_symbolStorage,
                            m_usedMacroAndSourceStorage,
                            m_sourceWatcher,
                            m_filePathCache,
                            m_fileStatusCache,
                            m_symbolStorage.m_database};
};

} // namespace ClangBackEnd
