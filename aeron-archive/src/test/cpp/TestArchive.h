/*
 * Copyright 2014-2023 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef AERON_TESTARCHIVE_H
#define AERON_TESTARCHIVE_H

using namespace aeron;
using namespace aeron::util;
using namespace aeron::concurrent;
using namespace aeron::archive::client;

static const std::chrono::duration<long, std::milli> IDLE_SLEEP_MS_1(1);

#ifdef _WIN32
static bool aeron_file_exists(const char *path)
{
    DWORD dwAttrib = GetFileAttributes(path);
    return dwAttrib != INVALID_FILE_ATTRIBUTES;
}

static int aeron_delete_directory(const char *dir)
{
    char dir_buffer[1024] = { 0 };

    size_t dir_length = strlen(dir);
    if (dir_length > (1024 - 2))
    {
        return -1;
    }

    memcpy(dir_buffer, dir, dir_length);
    dir_buffer[dir_length] = '\0';
    dir_buffer[dir_length + 1] = '\0';

    SHFILEOPSTRUCT file_op =
        {
            nullptr,
            FO_DELETE,
            dir_buffer,
            nullptr,
            FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT,
            false,
            nullptr,
            nullptr
        };

    return SHFileOperation(&file_op);
}
#else
static bool aeron_file_exists(const char *path)
{
    struct stat stat_info = {};
    return stat(path, &stat_info) == 0;
}

static int aeron_unlink_func(const char *path, const struct stat *sb, int type_flag, struct FTW *ftw)
{
    if (remove(path) != 0)
    {
        perror("remove");
    }

    return 0;
}

static int aeron_delete_directory(const char *dirname)
{
    return nftw(dirname, aeron_unlink_func, 64, FTW_DEPTH | FTW_PHYS);
}
#endif

class TestArchive
{
public:
    TestArchive(
        std::string aeronDir,
        std::string archiveDir,
        std::ostream& stream,
        std::string controlChannel = "aeron:udp?endpoint=localhost:8010",
        std::string replicationChannel = "aeron:udp?endpoint=localhost:0")
        : m_archiveDir(archiveDir), m_aeronDir(aeronDir), m_stream(stream)
    {
        m_stream << currentTimeMillis() << " [SetUp] Starting ArchivingMediaDriver..." << std::endl;

        std::string aeronDirArg = "-Daeron.dir=" + aeronDir;
        std::string archiveDirArg = "-Daeron.archive.dir=" + archiveDir;
        std::string controlChannelArg = "-Daeron.archive.control.channel=" + controlChannel;
        std::string replicationChannelArg = "-Daeron.archive.replication.channel=" + replicationChannel;
        const char *const argv[] =
        {
            "java",
#if JAVA_MAJOR_VERSION >= 9
            "--add-opens",
            "java.base/java.lang.reflect=ALL-UNNAMED",
            "--add-opens",
            "java.base/java.net=ALL-UNNAMED",
            "--add-opens",
            "java.base/sun.nio.ch=ALL-UNNAMED",
#endif
            "-Daeron.dir.delete.on.start=true",
            "-Daeron.dir.delete.on.shutdown=true",
            "-Daeron.archive.dir.delete.on.start=true",
            "-Daeron.archive.max.catalog.entries=128",
            "-Daeron.term.buffer.sparse.file=true",
            "-Daeron.perform.storage.checks=false",
            "-Daeron.term.buffer.length=64k",
            "-Daeron.ipc.term.buffer.length=64k",
            "-Daeron.threading.mode=SHARED",
            "-Daeron.shared.idle.strategy=yield",
            "-Daeron.archive.threading.mode=SHARED",
            "-Daeron.archive.idle.strategy=yield",
            "-Daeron.archive.recording.events.enabled=false",
            "-Daeron.driver.termination.validator=io.aeron.driver.DefaultAllowTerminationValidator",
            "-Daeron.archive.authenticator.supplier=io.aeron.samples.archive.SampleAuthenticatorSupplier",
            controlChannelArg.c_str(),
            replicationChannelArg.c_str(),
            "-Daeron.archive.control.response.channel=aeron:udp?endpoint=localhost:0",
            archiveDirArg.c_str(),
            aeronDirArg.c_str(),
            "-cp",
            m_aeronAllJar.c_str(),
            "io.aeron.archive.ArchivingMediaDriver",
            nullptr
        };

#if defined(_WIN32)
        m_pid = _spawnv(P_NOWAIT, m_java.c_str(), &argv[0]);
#else
        m_pid = -1;
        if (0 != posix_spawn(&m_pid, m_java.c_str(), nullptr, nullptr, (char * const *)&argv[0], nullptr))
        {
            perror("spawn");
            ::exit(EXIT_FAILURE);
        }
#endif

        if (m_pid < 0)
        {
            perror("spawn");
            ::exit(EXIT_FAILURE);
        }

        m_stream << currentTimeMillis() << " [SetUp] ArchivingMediaDriver PID " << m_pid << std::endl;
    }

    ~TestArchive()
    {
        if (0 != m_pid)
        {
            m_stream << currentTimeMillis() << " [TearDown] Shutting down PID " << m_pid << std::endl;

            const std::string aeronPath = m_aeronDir;
            const std::string cncFilename = aeronPath + std::string(1, AERON_FILE_SEP) + CncFileDescriptor::CNC_FILE;

            {
                const CncFileReader reader = aeron::CncFileReader::mapExisting(m_aeronDir.c_str());
                printErrors(aeronPath, m_stream);
            }

            if (aeron::Context::requestDriverTermination(aeronPath, nullptr, 0))
            {
                m_stream << currentTimeMillis() << " [TearDown] Waiting for driver termination" << std::endl;

                while (aeron_file_exists(cncFilename.c_str()))
                {
                    std::this_thread::sleep_for(IDLE_SLEEP_MS_1);
                }

                m_stream << currentTimeMillis() << " [TearDown] CnC file no longer exists" << std::endl;

#if defined(_WIN32)
                WaitForSingleObject(reinterpret_cast<HANDLE>(m_pid), INFINITE);
#else
                int process_status = -1;
                do
                {
                    waitpid(m_pid, &process_status, WUNTRACED);
                }
                while (0 >= WIFEXITED(process_status));
#endif
                m_stream << currentTimeMillis() << " [TearDown] Driver terminated" << std::endl;
            }
            else
            {
                const auto now_ms = currentTimeMillis();
                m_stream << now_ms << " [TearDown] Failed to send driver terminate command" << std::endl;
                m_stream << now_ms << " [TearDown] Deleting " << m_archiveDir << std::endl;
                if (aeron_delete_directory(m_archiveDir.c_str()) != 0)
                {
                    m_stream << currentTimeMillis() << " [TearDown] Failed to delete " << m_archiveDir << std::endl;
                }
            }
        }
    }

    static void printErrors(const std::string &aeronPath, std::ostream &out)
    {
        const CncFileReader reader = aeron::CncFileReader::mapExisting(aeronPath.c_str());

        int count = reader.readErrorLog(
            [&](
                std::int32_t observationCount,
                std::int64_t firstObservationTimestamp,
                std::int64_t lastObservationTimestamp,
                const std::string &encodedException)
            {
                out << "***\n" << observationCount
                         << " observations for:\n " << encodedException.c_str() << std::endl;
            },
            0);

        out << currentTimeMillis() << " [TearDown] " << count << " distinct errors observed." << std::endl;
    }

private:
    const std::string m_java = JAVA_EXECUTABLE;
    const std::string m_aeronAllJar = AERON_ALL_JAR;
    const std::string m_archiveDir;
    const std::string m_aeronDir;
    std::ostream &m_stream;
    pid_t m_pid = -1;
};

#endif //AERON_TESTARCHIVE_H
