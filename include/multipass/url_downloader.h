/*
 * Copyright (C) 2017-2021 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef MULTIPASS_URL_DOWNLOADER_H
#define MULTIPASS_URL_DOWNLOADER_H

#include <multipass/path.h>
#include <multipass/progress_monitor.h>
#include <multipass/singleton.h>

#include <QByteArray>
#include <QDateTime>
#include <QNetworkAccessManager>

#include <atomic>
#include <chrono>

#define MP_NETMGRFACTORY multipass::NetworkManagerFactory::instance()

class QUrl;
class QString;
namespace multipass
{
class NetworkManagerFactory : public Singleton<NetworkManagerFactory>
{
public:
    NetworkManagerFactory(const Singleton<NetworkManagerFactory>::PrivatePass&) noexcept;

    virtual std::unique_ptr<QNetworkAccessManager> make_network_manager(const Path& cache_dir_path);
};

class URLDownloader
{
public:
    URLDownloader(std::chrono::milliseconds timeout);
    URLDownloader(const Path& cache_dir, std::chrono::milliseconds timeout);
    virtual ~URLDownloader() = default;
    virtual void download_to(const QUrl& url, const QString& file_name, int64_t size, const int download_type,
                             const ProgressMonitor& monitor);
    virtual QByteArray download(const QUrl& url);
    virtual QDateTime last_modified(const QUrl& url);
    virtual void abort_all_downloads();

protected:
    std::atomic_bool abort_download{false};

private:
    URLDownloader(const URLDownloader&) = delete;
    URLDownloader& operator=(const URLDownloader&) = delete;

    const Path cache_dir_path;
    std::chrono::milliseconds timeout;
};
}
#endif // MULTIPASS_URL_DOWNLOADER_H
