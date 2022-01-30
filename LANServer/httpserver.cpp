#include "httpserver.h"
#include "qhttpengine/qobjecthandler.h"
#include "qhttpengine/handler.h"
#include "mediahandler.h"

#include "Common/network.h"
#include "Common/logger.h"
#include "Play/Playlist/playlist.h"
#include "Play/Danmu/common.h"
#include "Play/Danmu/Manager/danmumanager.h"
#include "Play/Danmu/Manager/pool.h"
#include "Play/Danmu/danmupool.h"
#include "Play/Video/mpvpreview.h"
#include "MediaLibrary/animeworker.h"
#include "Script/scriptmanager.h"
#include "Script/danmuscript.h"
#include "globalobjects.h"

#include <QCoreApplication>
#include <QMimeDatabase>
#include <QFileInfo>

namespace
{
    const QString duration2Str(int duration, bool hour = false)
    {
        int cmin=duration/60;
        int cls=duration-cmin*60;
        if(hour)
        {
            int ch = cmin/60;
            cmin = cmin - ch*60;
            return QString("%1:%2:%3").arg(ch, 2, 10, QChar('0')).arg(cmin,2,10,QChar('0')).arg(cls,2,10,QChar('0'));
        }
        else
        {
            return QString("%1:%2").arg(cmin,2,10,QChar('0')).arg(cls,2,10,QChar('0'));
        }
    }
}
HttpServer::HttpServer(QObject *parent) : QObject(parent)
{
    MediaHandler *handler=new MediaHandler(&mediaHash,this);
    const QString strApp(QCoreApplication::applicationDirPath()+"/web");
#ifdef CONFIG_UNIX_DATA
    const QString strHome(QDir::homePath()+"/.config/kikoplay/web");
    const QString strSys("/usr/share/kikoplay/web");

    const QFileInfo fileinfoHome(strHome);
    const QFileInfo fileinfoSys(strSys);
    const QFileInfo fileinfoApp(strApp);

    if (fileinfoHome.exists() || fileinfoHome.isDir()) {
        handler->setDocumentRoot(strHome);
    } else if (fileinfoSys.exists() || fileinfoSys.isDir()) {
        handler->setDocumentRoot(strSys);
    } else {
        handler->setDocumentRoot(strApp);
    }
#else
    handler->setDocumentRoot(strApp);
#endif
    handler->addRedirect(QRegExp("^$"), "/index.html");

    QHttpEngine::QObjectHandler *apiHandler=new QHttpEngine::QObjectHandler(this);
    apiHandler->registerMethod("playlist", this, &HttpServer::api_Playlist);
    apiHandler->registerMethod("updateTime", this, &HttpServer::api_UpdateTime);
    apiHandler->registerMethod("danmu/v3/", this, &HttpServer::api_Danmu);
    apiHandler->registerMethod("subtitle", this, &HttpServer::api_Subtitle);
    apiHandler->registerMethod("danmu/full/", this, &HttpServer::api_DanmuFull);
    apiHandler->registerMethod("updateDelay", this, &HttpServer::api_UpdateDelay);
    apiHandler->registerMethod("updateTimeline", this, &HttpServer::api_UpdateTimeline);
    apiHandler->registerMethod("screenshot", this, &HttpServer::api_Screenshot);
    apiHandler->registerMethod("danmu/launch", this, &HttpServer::api_Launch);
    handler->addSubHandler(QRegExp("api/"), apiHandler);

    server = new QHttpEngine::Server(handler,this);
    GlobalObjects::playlist->dumpJsonPlaylist(playlistDoc,mediaHash);
}

HttpServer::~HttpServer()
{
    server->close();
}

bool HttpServer::startServer(quint16 port)
{

   if(server->isListening())return true;
   bool r = server->listen(QHostAddress::AnyIPv4,port);
   Logger::logger()->log(Logger::LANServer, r?"Server start":server->errorString());
   if(!r) server->close();
   return r;
}

void HttpServer::stopServer()
{
    server->close();
    Logger::logger()->log(Logger::LANServer, "Server close");
}

void HttpServer::api_Playlist(QHttpEngine::Socket *socket)
{
    QMetaObject::invokeMethod(GlobalObjects::playlist,[this](){
        GlobalObjects::playlist->dumpJsonPlaylist(playlistDoc,mediaHash);
    },Qt::BlockingQueuedConnection);
    Logger::logger()->log(Logger::LANServer, QString("[%1]Playlist").arg(socket->peerAddress().toString()));

    QByteArray data = playlistDoc.toJson();
    QByteArray compressedBytes;
    Network::gzipCompress(data,compressedBytes);

    socket->setHeader("Content-Length", QByteArray::number(compressedBytes.length()));
    socket->setHeader("Content-Type", "application/json");
    socket->setHeader("Content-Encoding", "gzip");
    socket->writeHeaders();
    socket->write(compressedBytes);
    socket->close();
}

void HttpServer::api_UpdateTime(QHttpEngine::Socket *socket)
{
    bool syncPlayTime=GlobalObjects::appSetting->value("Server/SyncPlayTime",true).toBool();
    if(syncPlayTime)
    {
        QJsonDocument document;
        if (socket->readJson(document))
        {
            Logger::logger()->log(Logger::LANServer, QString("[%1]UpdateTime").arg(socket->peerAddress().toString()));
            QVariantMap data = document.object().toVariantMap();
            QString mediaPath=mediaHash.value(data.value("mediaId").toString());
            int playTime=data.value("playTime").toInt();
            PlayListItem::PlayState playTimeState=PlayListItem::PlayState(data.value("playTimeState").toInt());
            QMetaObject::invokeMethod(GlobalObjects::playlist,[mediaPath,playTime,playTimeState](){
                GlobalObjects::playlist->updatePlayTime(mediaPath,playTime,playTimeState);
            },Qt::QueuedConnection);
        }
    }
    socket->close();
}

void HttpServer::api_Danmu(QHttpEngine::Socket *socket)
{ 
    QString poolId=socket->queryString().value("id");
    bool update=(socket->queryString().value("update").toLower()=="true");
    Pool *pool=GlobalObjects::danmuManager->getPool(poolId);
    Logger::logger()->log(Logger::LANServer, QString("[%1]Danmu %2%3").arg(socket->peerAddress().toString(),
                                                   pool?pool->epTitle():"",
                                                   update?", update=true":""));
    QJsonArray danmuArray;
    if(pool)
    {
        if(update)
        {
            QList<QSharedPointer<DanmuComment> > incList;
            pool->update(-1,&incList);
            danmuArray=Pool::exportJson(incList);
        }
        else
        {
            danmuArray=pool->exportJson();
        }
    }
    QJsonObject resposeObj
    {
        {"code", 0},
        {"data", danmuArray},
        {"update",update}
    };
    QByteArray data = QJsonDocument(resposeObj).toJson();
    QByteArray compressedBytes;
    Network::gzipCompress(data,compressedBytes);
    socket->setHeader("Content-Length", QByteArray::number(compressedBytes.length()));
    socket->setHeader("Content-Type", "application/json");
    socket->setHeader("Content-Encoding", "gzip");
    socket->writeHeaders();
    socket->write(compressedBytes);
    socket->close();
}

void HttpServer::api_DanmuFull(QHttpEngine::Socket *socket)
{
    QString poolId=socket->queryString().value("id");
    bool update=(socket->queryString().value("update").toLower()=="true");
    Pool *pool=GlobalObjects::danmuManager->getPool(poolId);
    Logger::logger()->log(Logger::LANServer, QString("[%1]Danmu(Full) %2%3").arg(socket->peerAddress().toString(),
                                                        pool?pool->epTitle():"",
                                                        update?", update=true":""));
    QJsonObject resposeObj;
    if(pool)
    {
        if(update)
        {
            QList<QSharedPointer<DanmuComment> > incList;
            pool->update(-1,&incList);
            resposeObj=
            {
                {"comment", Pool::exportJson(incList, true)},
                {"update", true}
            };
        }
        else
        {
            resposeObj=pool->exportFullJson();
            resposeObj.insert("update", false);
            if(pool->sources().size()>0)
            {
                QJsonArray supportedScripts;
                QList<DanmuSource> sources;
                for(auto &src : pool->sources())
                    sources.append(src);
                for(auto &script : GlobalObjects::scriptManager->scripts(ScriptType::DANMU))
                {
                    DanmuScript *dmScript = static_cast<DanmuScript *>(script.data());
                    bool ret = false;
                    dmScript->hasSourceToLaunch(sources, ret);
                    if(ret) supportedScripts.append(dmScript->id());
                }
                if(supportedScripts.size()>0)
                    resposeObj.insert("launchScripts", supportedScripts);
            }
        }
    }
    QByteArray data = QJsonDocument(resposeObj).toJson();
    QByteArray compressedBytes;
    Network::gzipCompress(data,compressedBytes);
    socket->setHeader("Content-Length", QByteArray::number(compressedBytes.length()));
    socket->setHeader("Content-Type", "application/json");
    socket->setHeader("Content-Encoding", "gzip");
    socket->writeHeaders();
    socket->write(compressedBytes);
    socket->close();
}

void HttpServer::api_UpdateDelay(QHttpEngine::Socket *socket)
{
    QJsonDocument document;
    if (socket->readJson(document))
    {
        QVariantMap data = document.object().toVariantMap();
        QString poolId=data.value("danmuPool").toString();
        int delay=data.value("delay").toInt();  //ms
        int sourceId=data.value("source").toInt();
        Logger::logger()->log(Logger::LANServer, QString("[%1]UpdateDelay, SourceId: %2").arg(socket->peerAddress().toString(),QString::number(sourceId)));
        Pool *pool=GlobalObjects::danmuManager->getPool(poolId,false);
        if(pool) pool->setDelay(sourceId, delay);
    }
    socket->close();
}

void HttpServer::api_UpdateTimeline(QHttpEngine::Socket *socket)
{
    QJsonDocument document;
    if (socket->readJson(document))
    {
        QVariantMap data = document.object().toVariantMap();
        QString poolId=data.value("danmuPool").toString();
        QString timelineStr=data.value("timeline").toString();
        int sourceId=data.value("source").toInt();
        Logger::logger()->log(Logger::LANServer, QString("[%1]UpdateTimeline, SourceId: %2").arg(socket->peerAddress().toString(),QString::number(sourceId)));
        Pool *pool=GlobalObjects::danmuManager->getPool(poolId,false);
        DanmuSource srcInfo;
        srcInfo.setTimeline(timelineStr);
        if(pool) pool->setTimeline(sourceId, srcInfo.timelineInfo);
    }
    socket->close();
}

void HttpServer::api_Subtitle(QHttpEngine::Socket *socket)
{
    QString mediaId=socket->queryString().value("id");
    QString mediaPath=mediaHash.value(mediaId);
    QFileInfo fi(mediaPath);
    QString dir=fi.absolutePath(),name=fi.baseName();
    static QStringList supportedSubFormats={"","ass","ssa","srt"};
    Logger::logger()->log(Logger::LANServer, QString("[%1]Subtitle - %2").arg(socket->peerAddress().toString(),name));
    int formatIndex=0;
    for(int i=1;i<4;++i)
    {
        QFileInfo subInfo(dir,name+"."+supportedSubFormats[i]);
        if(subInfo.exists())
        {
            formatIndex=i;
            break;
        }
    }
    QJsonObject resposeObj
    {
        {"type", supportedSubFormats[formatIndex]}
    };
    QByteArray data = QJsonDocument(resposeObj).toJson();
    socket->setHeader("Content-Length", QByteArray::number(data.length()));
    socket->setHeader("Content-Type", "application/json");
    socket->writeHeaders();
    socket->write(data);
    socket->close();
}

void HttpServer::api_Screenshot(QHttpEngine::Socket *socket)
{
    QJsonDocument document;
    if (socket->readJson(document))
    {
        QVariantMap data = document.object().toVariantMap();
        QString animeName=data.value("animeName").toString();
        double pos = data.value("pos").toDouble();  //s
        QString mediaId=data.value("mediaId").toString();
        QString mediaPath=mediaHash.value(mediaId);
        QFileInfo fi(mediaPath);
        if(fi.exists() && !animeName.isEmpty())
        {
            QTemporaryFile tmpImg("XXXXXX.jpg");
            if(tmpImg.open())
            {
                int cmin=pos/60;
                int cls=pos-cmin*60;
                QString posStr(QString("%1:%2").arg(cmin,2,10,QChar('0')).arg(cls,2,10,QChar('0')));

                QString ffmpegPath = GlobalObjects::appSetting->value("Play/FFmpeg", "ffmpeg").toString();
                QStringList arguments;
                arguments << "-ss" << QString::number(pos);
                arguments << "-i" << mediaPath;
                arguments << "-vframes" << "1";
                arguments << "-y";
                arguments << tmpImg.fileName();

                QProcess ffmpegProcess;
                QEventLoop eventLoop;
                bool success = true;
                QString errorInfo;
                QObject::connect(&ffmpegProcess, &QProcess::errorOccurred, this, [&errorInfo, &eventLoop, &success](QProcess::ProcessError error){
                    if(error == QProcess::FailedToStart)
                    {
                        errorInfo = tr("Start FFmpeg Failed");
                    }
                   success = false;
                   eventLoop.quit();
                });
                QObject::connect(&ffmpegProcess, (void (QProcess:: *)(int, QProcess::ExitStatus))&QProcess::finished, this,
                                 [&errorInfo, &eventLoop, &success](int exitCode, QProcess::ExitStatus exitStatus){
                   success = (exitStatus == QProcess::NormalExit && exitCode == 0);
                   if(!success)
                   {
                       errorInfo = tr("Generate Failed, FFmpeg exit code: %1").arg(exitCode);
                   }
                   eventLoop.quit();
                });
                QObject::connect(&ffmpegProcess, &QProcess::readyReadStandardOutput, this, [&](){
                   qInfo()<<ffmpegProcess.readAllStandardOutput();
                });
                QObject::connect(&ffmpegProcess, &QProcess::readyReadStandardError, this, [&]() {
                    QString content(ffmpegProcess.readAllStandardError());
                    qInfo() << content.replace("\\n", "\n");
                });

                QTimer::singleShot(0, [&]() {
                    ffmpegProcess.start(ffmpegPath, arguments);
                });
                eventLoop.exec();
                if(success)
                {
                    QImage captureImage(tmpImg.fileName());
                    if(data.contains("duration")) //snippet task
                    {
                        qint64 timeId = QDateTime::currentDateTime().toMSecsSinceEpoch();
                        QString snippetPath(GlobalObjects::appSetting->value("Play/SnippetPath", GlobalObjects::dataPath + "/snippet").toString());
                        QDir dir;
                        if(!dir.exists(snippetPath)) dir.mkpath(snippetPath);
                        QString fileName(QString("%1/%2.%3").arg(snippetPath, QString::number(timeId), fi.suffix()));
                        QString duration = QString::number(qBound<int>(1, data.value("duration", 1).toInt(), 15));
                        QStringList arguments;
                        arguments << "-ss" << QString::number(pos);
                        arguments << "-i" << mediaPath;
                        arguments << "-t" << duration;
                        if(!data.value("retainAudio", true).toBool())
                            arguments << "-an";
                        arguments << "-y";
                        arguments << fileName;
                        QTimer::singleShot(0, [&]() {
                            ffmpegProcess.start(ffmpegPath, arguments);
                        });
                        eventLoop.exec();
                        if(success)
                        {
                            Logger::logger()->log(Logger::LANServer, QString("[%1]Snippet,[%2]%3").arg(socket->peerAddress().toString(),posStr, fi.filePath()));
                            QString info = data.value("info").toString();
                            if(info.isEmpty())  info = QString("%1,%2s - %3").arg(duration2Str(pos), duration, fi.fileName());
                            AnimeWorker::instance()->saveSnippet(animeName, info, timeId, captureImage);
                        }
                    }
                    else
                    {
                        Logger::logger()->log(Logger::LANServer, QString("[%1]Screenshot,[%2]%3").arg(socket->peerAddress().toString(),posStr, fi.filePath()));
                        QString info = data.value("info").toString();
                        if(info.isEmpty())  info = QString("%1 - %2").arg(duration2Str(pos), fi.fileName());
                        AnimeWorker::instance()->saveCapture(animeName, info, captureImage);
                    }
                }
                else
                {
                    Logger::logger()->log(Logger::LANServer, QString("[%1]Screenshot, [%2]%3, %4").arg(socket->peerAddress().toString(),posStr, fi.filePath(), errorInfo));
                }
            }
        }
    }
    socket->close();
}

void HttpServer::api_Launch(QHttpEngine::Socket *socket)
{
    QJsonDocument document;
    if (socket->readJson(document))
    {
        QVariantMap data = document.object().toVariantMap();
        Pool *pool=GlobalObjects::danmuManager->getPool(data.value("danmuPool").toString());
        QString text = data.value("text").toString();

        if(pool && !text.isEmpty())
        {

            int time = data.value("time").toInt();  //ms
            int color = data.value("color", 0xffffff).toInt();
            int fontsize = data.value("fontsize", int(DanmuComment::FontSizeLevel::Normal)).toInt();
            QString dateStr = data.value("date").toString();
            long long date;
            if(dateStr.isEmpty()) date = QDateTime::currentDateTime().toSecsSinceEpoch();
            else date = dateStr.toLongLong();
            int type = data.value("type", int(DanmuComment::DanmuType::Rolling)).toInt();

            DanmuComment comment;
            comment.text = text;
            comment.originTime = comment.time = time;
            comment.color = color;
            comment.fontSizeLevel = (DanmuComment::FontSizeLevel)fontsize;
            comment.date = date;
            comment.type = (DanmuComment::DanmuType)type;

            if(GlobalObjects::danmuPool->getPool()==pool)
            {
                int poolTime = GlobalObjects::danmuPool->getCurrentTime();
                if(time < 0)  // launch to current
                {
                    time = poolTime;
                    comment.originTime = comment.time = time;
                }
                if(qAbs(poolTime - time)<3000)
                {
                    GlobalObjects::danmuPool->launch({QSharedPointer<DanmuComment>(new DanmuComment(comment))});
                }
            }
            int cmin=time/1000/60;
            int cls=time/1000-cmin*60;
            QString commentInfo(QString("[%1:%2]%3").arg(cmin,2,10,QChar('0')).arg(cls,2,10,QChar('0')).arg(text));
            QString poolInfo(QString("%1 %2").arg(pool->animeTitle(), pool->toEp().toString()));
            Logger::logger()->log(Logger::LANServer, QString("[%1]Launch, [%2] %3").arg(socket->peerAddress().toString(), poolInfo, commentInfo));
            QStringList scriptIds = data.value("launchScripts").toStringList();
            if(time >= 0 && !scriptIds.isEmpty() && !pool->sources().isEmpty())
            {
                QList<DanmuSource> sources;
                for(auto &src : pool->sources())
                    sources.append(src);

                QStringList results;
                for(auto &id : scriptIds)
                {
                    auto script =  GlobalObjects::scriptManager->getScript(id);
                    DanmuScript *dmScript = static_cast<DanmuScript *>(script.data());
                    ScriptState state = dmScript->launch(sources, &comment);
                    results.append(QString("[%1]: %2").arg(id, state?tr("Success"):tr("Faild, %1").arg(state.info)));
                }
                QString msg(QString("%1\n%2\n%3").arg(poolInfo, commentInfo, results.join('\n')));
                Logger::logger()->log(Logger::Script, msg);
            }
        }
    }
    socket->close();
}
