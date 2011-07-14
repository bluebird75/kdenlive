/***************************************************************************
                        krender.cpp  -  description
                           -------------------
  begin                : Fri Nov 22 2002
  copyright            : (C) 2002 by Jason Wood
  email                : jasonwood@blueyonder.co.uk
  copyright            : (C) 2005 Lucio Flavio Correa
  email                : lucio.correa@gmail.com
  copyright            : (C) Marco Gittler
  email                : g.marco@freenet.de
  copyright            : (C) 2006 Jean-Baptiste Mardelle
  email                : jb@kdenlive.org

***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/


#include "renderer.h"
#include "kdenlivesettings.h"
#include "kthumb.h"
#include "definitions.h"
#include "slideshowclip.h"
#include "profilesdialog.h"
#include "blackmagic/devices.h"

#include <mlt++/Mlt.h>

#include <KDebug>
#include <KStandardDirs>
#include <KMessageBox>
#include <KLocale>
#include <KTemporaryFile>

#include <QTimer>
#include <QDir>
#include <QString>
#include <QApplication>

#include <cstdlib>
#include <cstdarg>

#include <QDebug>

static void kdenlive_callback(void* /*ptr*/, int level, const char* fmt, va_list vl)
{
    if (level > MLT_LOG_ERROR) return;
    QString error;
    QApplication::postEvent(qApp->activeWindow(), new MltErrorEvent(error.vsprintf(fmt, vl).simplified()));
    va_end(vl);
}


static void consumer_frame_show(mlt_consumer, Render * self, mlt_frame frame_ptr)
{
    // detect if the producer has finished playing. Is there a better way to do it?
    if (self->m_isBlocked) return;
    Mlt::Frame frame(frame_ptr);
    if (!frame.is_valid()) return;
    self->emitFrameNumber(mlt_frame_get_position(frame_ptr));
    if (self->sendFrameForAnalysis && frame_ptr->convert_image) {
        self->emitFrameUpdated(frame);
    }
    if (self->analyseAudio) {
        self->showAudio(frame);
    }
    if (frame.get_double("_speed") < 0.0 && mlt_frame_get_position(frame_ptr) <= 0) {
        self->pause();
        self->emitConsumerStopped();
    }
}


static void consumer_paused(mlt_consumer, Render * self, mlt_frame /*frame_ptr*/)
{
    // detect if the producer has finished playing. Is there a better way to do it?
    if (self->m_isBlocked) return;
    self->emitConsumerStopped();
}


static void consumer_gl_frame_show(mlt_consumer, Render * self, mlt_frame frame_ptr)
{
    // detect if the producer has finished playing. Is there a better way to do it?
    if (self->m_isBlocked) return;
    Mlt::Frame frame(frame_ptr);
    self->showFrame(frame);
    if (frame.get_double("_speed") == 0.0) {
        self->emitConsumerStopped();
    } else if (frame.get_double("_speed") < 0.0 && mlt_frame_get_position(frame_ptr) <= 0) {
        self->pause();
        self->emitConsumerStopped();
    }
}

Render::Render(const QString & rendererName, int winid, QString profile, QWidget *parent) :
    AbstractRender(rendererName, parent),
    m_isBlocked(0),
    analyseAudio(KdenliveSettings::monitor_audio()),
    m_name(rendererName),
    m_mltConsumer(NULL),
    m_mltProducer(NULL),
    m_mltProfile(NULL),
    m_framePosition(0),
    m_externalConsumer(false),
    m_isZoneMode(false),
    m_isLoopMode(false),
    m_isSplitView(false),
    m_blackClip(NULL),
    m_winid(winid)
{
    if (profile.isEmpty()) profile = KdenliveSettings::current_profile();
    buildConsumer(profile);

    m_mltProducer = m_blackClip->cut(0, 50);
    m_mltConsumer->connect(*m_mltProducer);
    m_mltProducer->set_speed(0.0);
}

Render::~Render()
{
    m_isBlocked = 1;
    closeMlt();
    delete m_mltProfile;
}


void Render::closeMlt()
{       
    //delete m_osdTimer;
    if (m_mltConsumer) delete m_mltConsumer;
    if (m_mltProducer) delete m_mltProducer;
    /*if (m_mltProducer) {
        Mlt::Service service(m_mltProducer->parent().get_service());
        mlt_service_lock(service.get_service());

        if (service.type() == tractor_type) {
            Mlt::Tractor tractor(service);
            Mlt::Field *field = tractor.field();
            mlt_service nextservice = mlt_service_get_producer(service.get_service());
            mlt_service nextservicetodisconnect;
            mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
            QString mlt_type = mlt_properties_get(properties, "mlt_type");
            QString resource = mlt_properties_get(properties, "mlt_service");
            // Delete all transitions
            while (mlt_type == "transition") {
                nextservicetodisconnect = nextservice;
                nextservice = mlt_service_producer(nextservice);
                mlt_field_disconnect_service(field->get_field(), nextservicetodisconnect);
                if (nextservice == NULL) break;
                properties = MLT_SERVICE_PROPERTIES(nextservice);
                mlt_type = mlt_properties_get(properties, "mlt_type");
                resource = mlt_properties_get(properties, "mlt_service");
            }

            delete field;
            field = NULL;
        }
        mlt_service_unlock(service.get_service());
    }*/

    kDebug() << "// // // CLOSE RENDERER " << m_name;
    if (m_blackClip) delete m_blackClip;
    //delete m_osdInfo;
}

void Render::slotSwitchFullscreen()
{
    if (m_mltConsumer) m_mltConsumer->set("full_screen", 1);
}

void Render::buildConsumer(const QString profileName)
{
    delete m_blackClip;
    m_blackClip = NULL;

    //TODO: uncomment following line when everything is clean
    // uncommented Feb 2011 --Granjow
    if (m_mltProfile) delete m_mltProfile;
    m_activeProfile = profileName;
    char *tmp = qstrdup(m_activeProfile.toUtf8().constData());
    setenv("MLT_PROFILE", tmp, 1);
    m_mltProfile = new Mlt::Profile(tmp);
    m_mltProfile->set_explicit(true);
    delete[] tmp;

    m_blackClip = new Mlt::Producer(*m_mltProfile, "colour", "black");
    m_blackClip->set("id", "black");
    m_blackClip->set("mlt_type", "producer");

    if (KdenliveSettings::external_display() && m_name != "clip") {
        // Use blackmagic card for video output
        QMap< QString, QString > profileProperties = ProfilesDialog::getSettingsFromFile(profileName);
        int device = KdenliveSettings::blackmagic_output_device();
        if (device >= 0) {
            if (BMInterface::isSupportedProfile(device, profileProperties)) {
                QString decklink = "decklink:" + QString::number(KdenliveSettings::blackmagic_output_device());
                tmp = qstrdup(decklink.toUtf8().constData());
                m_mltConsumer = new Mlt::Consumer(*m_mltProfile, tmp);
                delete[] tmp;
                if (m_mltConsumer->is_valid()) {
                    m_externalConsumer = true;
                    m_mltConsumer->listen("consumer-frame-show", this, (mlt_listener) consumer_frame_show);
                    m_mltConsumer->set("terminate_on_pause", 0);
                    m_mltConsumer->set("buffer", 12);
                    m_mltConsumer->set("deinterlace_method", "onefield");
                    m_mltConsumer->set("real_time", KdenliveSettings::mltthreads());
                    mlt_log_set_callback(kdenlive_callback);
                }
                if (m_mltConsumer && m_mltConsumer->is_valid()) return;
            } else KMessageBox::informationList(qApp->activeWindow(), i18n("Your project's profile %1 is not compatible with the blackmagic output card. Please see supported profiles below. Switching to normal video display.", m_mltProfile->description()), BMInterface::supportedModes(KdenliveSettings::blackmagic_output_device()));
        }
    }
    m_externalConsumer = false;
    QString videoDriver = KdenliveSettings::videodrivername();
    if (!videoDriver.isEmpty()) {
        if (videoDriver == "x11_noaccel") {
            setenv("SDL_VIDEO_YUV_HWACCEL", "0", 1);
            videoDriver = "x11";
        } else {
            unsetenv("SDL_VIDEO_YUV_HWACCEL");
        }
    }
    setenv("SDL_VIDEO_ALLOW_SCREENSAVER", "1", 1);

    //m_mltConsumer->set("fullscreen", 1);
    if (m_winid == 0) {
        // OpenGL monitor
        m_mltConsumer = new Mlt::Consumer(*m_mltProfile, "sdl_audio");
        m_mltConsumer->set("preview_off", 1);
        m_mltConsumer->set("preview_format", mlt_image_rgb24a);
        m_mltConsumer->listen("consumer-frame-show", this, (mlt_listener) consumer_gl_frame_show);
    } else {
        m_mltConsumer = new Mlt::Consumer(*m_mltProfile, "sdl_preview");
        // FIXME: the event object returned by the listen gets leaked...
        m_mltConsumer->listen("consumer-frame-show", this, (mlt_listener) consumer_frame_show);
        m_mltConsumer->listen("consumer-sdl-paused", this, (mlt_listener) consumer_paused);
        m_mltConsumer->set("window_id", m_winid);
    }
    m_mltConsumer->set("resize", 1);
    m_mltConsumer->set("terminate_on_pause", 1);
    m_mltConsumer->set("window_background", KdenliveSettings::window_background().name().toUtf8().constData());
    m_mltConsumer->set("rescale", "nearest");
    mlt_log_set_callback(kdenlive_callback);

    QString audioDevice = KdenliveSettings::audiodevicename();
    if (!audioDevice.isEmpty())
        m_mltConsumer->set("audio_device", audioDevice.toUtf8().constData());

    if (!videoDriver.isEmpty())
        m_mltConsumer->set("video_driver", videoDriver.toUtf8().constData());

    QString audioDriver = KdenliveSettings::audiodrivername();

    /*
    // Disabled because the "auto" detected driver was sometimes wrong
    if (audioDriver.isEmpty())
        audioDriver = KdenliveSettings::autoaudiodrivername();
    */

    if (!audioDriver.isEmpty())
        m_mltConsumer->set("audio_driver", audioDriver.toUtf8().constData());

    m_mltConsumer->set("progressive", 1);
    m_mltConsumer->set("audio_buffer", 1024);
    m_mltConsumer->set("frequency", 48000);
    m_mltConsumer->set("real_time", KdenliveSettings::mltthreads());
}

Mlt::Producer *Render::invalidProducer(const QString &id)
{
    Mlt::Producer *clip;
    QString txt = "+" + i18n("Missing clip") + ".txt";
    char *tmp = qstrdup(txt.toUtf8().constData());
    clip = new Mlt::Producer(*m_mltProfile, tmp);
    delete[] tmp;
    if (clip == NULL) clip = new Mlt::Producer(*m_mltProfile, "colour", "red");
    else {
        clip->set("bgcolour", "0xff0000ff");
        clip->set("pad", "10");
    }
    clip->set("id", id.toUtf8().constData());
    clip->set("mlt_type", "producer");
    return clip;
}

int Render::resetProfile(const QString profileName, bool dropSceneList)
{
    QString scene;
    if (!dropSceneList) scene = sceneList();
    if (m_mltConsumer) {
        if (m_externalConsumer == KdenliveSettings::external_display()) {
            if (KdenliveSettings::external_display() && m_activeProfile == profileName) return 1;
            QString videoDriver = KdenliveSettings::videodrivername();
            QString currentDriver = m_mltConsumer->get("video_driver");
            if (getenv("SDL_VIDEO_YUV_HWACCEL") != NULL && currentDriver == "x11") currentDriver = "x11_noaccel";
            QString background = KdenliveSettings::window_background().name();
            QString currentBackground = m_mltConsumer->get("window_background");
            if (m_activeProfile == profileName && currentDriver == videoDriver && background == currentBackground) {
                kDebug() << "reset to same profile, nothing to do";
                return 1;
            }
        }

        if (m_isSplitView) slotSplitView(false);
        if (!m_mltConsumer->is_stopped()) m_mltConsumer->stop();
        m_mltConsumer->purge();
        delete m_mltConsumer;
        m_mltConsumer = NULL;
    }
    int pos = 0;
    double current_fps = m_mltProfile->fps();
    double current_dar = m_mltProfile->dar();
    delete m_blackClip;
    m_blackClip = NULL;

    if (m_mltProducer) {
        pos = m_mltProducer->position();

        Mlt::Service service(m_mltProducer->get_service());
        if (service.type() == tractor_type) {
            Mlt::Tractor tractor(service);
            for (int trackNb = tractor.count() - 1; trackNb >= 0; --trackNb) {
                Mlt::Producer trackProducer(tractor.track(trackNb));
                Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
                trackPlaylist.clear();
            }
        }

        delete m_mltProducer;
    }
    m_mltProducer = NULL;
    buildConsumer(profileName);
    double new_fps = m_mltProfile->fps();
    double new_dar = m_mltProfile->dar();
    
    if (!dropSceneList) {
        // We need to recover our playlist
        if (current_fps != new_fps) {
            // fps changed, we must update the scenelist positions
            scene = updateSceneListFps(current_fps, new_fps, scene);
        }
        setSceneList(scene, pos);
        // producers have changed (different profile), so reset them...
        emit refreshDocumentProducers(new_dar != current_dar, current_fps != new_fps);
    }
    return 1;
}

void Render::seek(GenTime time)
{
    if (!m_mltProducer)
        return;
    m_isBlocked = false;
    m_mltProducer->seek((int)(time.frames(m_fps)));
    refresh();
}

void Render::seek(int time)
{
    if (!m_mltProducer)
        return;
    m_isBlocked = false;
    m_mltProducer->seek(time);
    refresh();
}

//static
/*QPixmap Render::frameThumbnail(Mlt::Frame *frame, int width, int height, bool border) {
    QPixmap pix(width, height);

    mlt_image_format format = mlt_image_rgb24a;
    uint8_t *thumb = frame->get_image(format, width, height);
    QImage image(thumb, width, height, QImage::Format_ARGB32);

    if (!image.isNull()) {
        pix = pix.fromImage(image);
        if (border) {
            QPainter painter(&pix);
            painter.drawRect(0, 0, width - 1, height - 1);
        }
    } else pix.fill(Qt::black);
    return pix;
}
*/
int Render::frameRenderWidth() const
{
    return m_mltProfile->width();
}

int Render::renderWidth() const
{
    return (int)(m_mltProfile->height() * m_mltProfile->dar() + 0.5);
}

int Render::renderHeight() const
{
    return m_mltProfile->height();
}

QImage Render::extractFrame(int frame_position, QString path, int width, int height)
{
    if (width == -1) {
        width = renderWidth();
        height = renderHeight();
    } else if (width % 2 == 1) width++;

    if (!path.isEmpty()) {
        Mlt::Producer *producer = new Mlt::Producer(*m_mltProfile, path.toUtf8().constData());
        if (producer) {
            if (producer->is_valid()) {
                QImage img = KThumb::getFrame(producer, frame_position, width, height);
                delete producer;
                return img;
            }
            else delete producer;
        }
    }
    
    if (!m_mltProducer || !path.isEmpty()) {
        QImage pix(width, height, QImage::Format_RGB32);
        pix.fill(Qt::black);
        return pix;
    }
    return KThumb::getFrame(m_mltProducer, frame_position, width, height);
}

QPixmap Render::getImageThumbnail(KUrl url, int /*width*/, int /*height*/)
{
    QImage im;
    QPixmap pixmap;
    if (url.fileName().startsWith(".all.")) {  //  check for slideshow
        QString fileType = url.fileName().right(3);
        QStringList more;
        QStringList::Iterator it;

        QDir dir(url.directory());
        QStringList filter;
        filter << "*." + fileType;
        filter << "*." + fileType.toUpper();
        more = dir.entryList(filter, QDir::Files);
        im.load(url.directory() + '/' + more.at(0));
    } else im.load(url.path());
    //pixmap = im.scaled(width, height);
    return pixmap;
}

double Render::consumerRatio() const
{
    if (!m_mltConsumer) return 1.0;
    return (m_mltConsumer->get_double("aspect_ratio_num") / m_mltConsumer->get_double("aspect_ratio_den"));
}


int Render::getLength()
{

    if (m_mltProducer) {
        // kDebug()<<"//////  LENGTH: "<<mlt_producer_get_playtime(m_mltProducer->get_producer());
        return mlt_producer_get_playtime(m_mltProducer->get_producer());
    }
    return 0;
}

bool Render::isValid(KUrl url)
{
    Mlt::Producer producer(*m_mltProfile, url.path().toUtf8().constData());
    if (producer.is_blank())
        return false;

    return true;
}

double Render::dar() const
{
    return m_mltProfile->dar();
}

double Render::sar() const
{
    return m_mltProfile->sar();
}

void Render::slotSplitView(bool doit)
{
    m_isSplitView = doit;
    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);
    if (service.type() != tractor_type || tractor.count() < 2) return;
    Mlt::Field *field = tractor.field();
    if (doit) {
        for (int i = 1, screen = 0; i < tractor.count() && screen < 4; i++) {
            Mlt::Producer trackProducer(tractor.track(i));
            kDebug() << "// TRACK: " << i << ", HIDE: " << trackProducer.get("hide");
            if (QString(trackProducer.get("hide")).toInt() != 1) {
                kDebug() << "// ADIDNG TRACK: " << i;
                Mlt::Transition *transition = new Mlt::Transition(*m_mltProfile, "composite");
                transition->set("mlt_service", "composite");
                transition->set("a_track", 0);
                transition->set("b_track", i);
                transition->set("distort", 1);
                transition->set("internal_added", "200");
                const char *tmp;
                switch (screen) {
                case 0:
                    tmp = "0,0:50%x50%";
                    break;
                case 1:
                    tmp = "50%,0:50%x50%";
                    break;
                case 2:
                    tmp = "0,50%:50%x50%";
                    break;
                case 3:
                default:
                    tmp = "50%,50%:50%x50%";
                    break;
                }
                transition->set("geometry", tmp);
                transition->set("always_active", "1");
                field->plant_transition(*transition, 0, i);
                //delete[] tmp;
                screen++;
            }
        }
        m_mltConsumer->set("refresh", 1);
    } else {
        mlt_service serv = m_mltProducer->parent().get_service();
        mlt_service nextservice = mlt_service_get_producer(serv);
        mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
        QString mlt_type = mlt_properties_get(properties, "mlt_type");
        QString resource = mlt_properties_get(properties, "mlt_service");

        while (mlt_type == "transition") {
            QString added = mlt_properties_get(MLT_SERVICE_PROPERTIES(nextservice), "internal_added");
            if (added == "200") {
                mlt_field_disconnect_service(field->get_field(), nextservice);
            }
            nextservice = mlt_service_producer(nextservice);
            if (nextservice == NULL) break;
            properties = MLT_SERVICE_PROPERTIES(nextservice);
            mlt_type = mlt_properties_get(properties, "mlt_type");
            resource = mlt_properties_get(properties, "mlt_service");
            m_mltConsumer->set("refresh", 1);
        }
    }
}

void Render::getFileProperties(const QDomElement xml, const QString &clipId, int imageHeight, bool replaceProducer, bool selectClip)
{
    QString path;
    QLocale locale;
    bool proxyProducer;
    if (xml.hasAttribute("proxy") && xml.attribute("proxy") != "-") {
        path = xml.attribute("proxy");
        proxyProducer = true;
    }
    else {
        path = xml.attribute("resource");
        proxyProducer = false;
    }

    KUrl url(path);
    Mlt::Producer *producer = NULL;
    CLIPTYPE type = (CLIPTYPE)xml.attribute("type").toInt();
    //kDebug() << "PROFILE WIDT: "<< xml.attribute("mlt_service") << ": "<< m_mltProfile->width() << "\n...................\n\n";
    /*if (xml.attribute("type").toInt() == TEXT && !QFile::exists(url.path())) {
        emit replyGetFileProperties(clipId, producer, QMap < QString, QString >(), QMap < QString, QString >(), replaceProducer);
        return;
    }*/

    if (type == COLOR) {
        producer = new Mlt::Producer(*m_mltProfile, 0, ("colour:" + xml.attribute("colour")).toUtf8().constData());
    } else if (type == TEXT) {
        producer = new Mlt::Producer(*m_mltProfile, 0, ("kdenlivetitle:" + xml.attribute("resource")).toUtf8().constData());
        if (producer && producer->is_valid() && xml.hasAttribute("xmldata"))
            producer->set("xmldata", xml.attribute("xmldata").toUtf8().constData());
    } else if (url.isEmpty()) {
        QDomDocument doc;
        QDomElement mlt = doc.createElement("mlt");
        QDomElement play = doc.createElement("playlist");
        doc.appendChild(mlt);
        mlt.appendChild(play);
        play.appendChild(doc.importNode(xml, true));
        producer = new Mlt::Producer(*m_mltProfile, "xml-string", doc.toString().toUtf8().constData());
    } else {
        char *resTag = qstrdup(QString("nocache:" + path).toUtf8().constData());
        producer = new Mlt::Producer(*m_mltProfile, path.toUtf8().constData());
        delete[] resTag;
    }


    if (producer == NULL || producer->is_blank() || !producer->is_valid()) {
        kDebug() << " / / / / / / / / ERROR / / / / // CANNOT LOAD PRODUCER: "<<path;
        if (proxyProducer) {
            // Proxy file is corrupted
            emit removeInvalidProxy(clipId, false);
        }
        else emit removeInvalidClip(clipId, replaceProducer);
        delete producer;
        return;
    }

    if (proxyProducer && xml.hasAttribute("proxy_out")) {
        producer->set("length", xml.attribute("proxy_out").toInt() + 1);
        producer->set("out", xml.attribute("proxy_out").toInt());
        if (producer->get_out() != xml.attribute("proxy_out").toInt()) {
            // Proxy file length is different than original clip length, this will corrupt project so disable this proxy clip
            emit removeInvalidProxy(clipId, true);
            delete producer;
            return;
        }
    }

    if (xml.hasAttribute("force_aspect_ratio")) {
        double aspect = xml.attribute("force_aspect_ratio").toDouble();
        if (aspect > 0) producer->set("force_aspect_ratio", aspect);
    }

    if (xml.hasAttribute("force_aspect_num") && xml.hasAttribute("force_aspect_den")) {
        int width = xml.attribute("frame_size").section('x', 0, 0).toInt();
        int height = xml.attribute("frame_size").section('x', 1, 1).toInt();
        int aspectNumerator = xml.attribute("force_aspect_num").toInt();
        int aspectDenominator = xml.attribute("force_aspect_den").toInt();
        if (aspectDenominator != 0 && width != 0)
            producer->set("force_aspect_ratio", double(height) * aspectNumerator / aspectDenominator / width);
    }

    if (xml.hasAttribute("force_fps")) {
        double fps = xml.attribute("force_fps").toDouble();
        if (fps > 0) producer->set("force_fps", fps);
    }

    if (xml.hasAttribute("force_progressive")) {
        bool ok;
        int progressive = xml.attribute("force_progressive").toInt(&ok);
        if (ok) producer->set("force_progressive", progressive);
    }
    if (xml.hasAttribute("force_tff")) {
        bool ok;
        int fieldOrder = xml.attribute("force_tff").toInt(&ok);
        if (ok) producer->set("force_tff", fieldOrder);
    }
    if (xml.hasAttribute("threads")) {
        int threads = xml.attribute("threads").toInt();
        if (threads != 1) producer->set("threads", threads);
    }
    if (xml.hasAttribute("video_index")) {
        int vindex = xml.attribute("video_index").toInt();
        if (vindex != 0) producer->set("video_index", vindex);
    }
    if (xml.hasAttribute("audio_index")) {
        int aindex = xml.attribute("audio_index").toInt();
        if (aindex != 0) producer->set("audio_index", aindex);
    }
    if (xml.hasAttribute("force_colorspace")) {
        int colorspace = xml.attribute("force_colorspace").toInt();
        if (colorspace != 0) producer->set("force_colorspace", colorspace);
    }
    if (xml.hasAttribute("full_luma")) {
        int full_luma = xml.attribute("full_luma").toInt();
        if (full_luma != 0) producer->set("set.force_full_luma", full_luma);
    }

    int clipOut = 0;
    int duration = 0;
    if (xml.hasAttribute("out")) clipOut = xml.attribute("out").toInt();

    // setup length here as otherwise default length (currently 15000 frames in MLT) will be taken even if outpoint is larger
    if (type == COLOR || type == TEXT || type == IMAGE || type == SLIDESHOW) {
        int length;
        if (xml.hasAttribute("length")) {
            if (clipOut > 0) duration = clipOut + 1;
            length = xml.attribute("length").toInt();
            clipOut = length - 1;
        }
        else length = xml.attribute("out").toInt() - xml.attribute("in").toInt();
        producer->set("length", length);
    }

    if (clipOut > 0) producer->set_in_and_out(xml.attribute("in").toInt(), clipOut);

    producer->set("id", clipId.toUtf8().constData());

    if (xml.hasAttribute("templatetext"))
        producer->set("templatetext", xml.attribute("templatetext").toUtf8().constData());

    if ((!replaceProducer && xml.hasAttribute("file_hash")) || xml.hasAttribute("proxy")) {
        // Clip  already has all properties
        if (replaceProducer) emit blockClipMonitor(clipId);
        // Querying a frame is required by MLT, otherwise the producer is not correctly initialised
        //Mlt::Frame *frame = producer->get_frame();
        //delete frame;
        emit replyGetFileProperties(clipId, producer, QMap < QString, QString >(), QMap < QString, QString >(), replaceProducer, selectClip);
        return;
    }

    int width = (int)(imageHeight * m_mltProfile->dar() + 0.5);
    QMap < QString, QString > filePropertyMap;
    QMap < QString, QString > metadataPropertyMap;

    int frameNumber = xml.attribute("thumbnail", "0").toInt();
    if (frameNumber != 0) producer->seek(frameNumber);

    filePropertyMap["duration"] = QString::number(duration > 0 ? duration : producer->get_playtime());
    //kDebug() << "///////  PRODUCER: " << url.path() << " IS: " << producer->get_playtime();

    Mlt::Frame *frame = producer->get_frame();

    if (type == SLIDESHOW) {
        int ttl = xml.hasAttribute("ttl") ? xml.attribute("ttl").toInt() : 0;
        if (ttl) producer->set("ttl", ttl);
        if (!xml.attribute("animation").isEmpty()) {
            Mlt::Filter *filter = new Mlt::Filter(*m_mltProfile, "affine");
            if (filter && filter->is_valid()) {
                int cycle = ttl;
                QString geometry = SlideshowClip::animationToGeometry(xml.attribute("animation"), cycle);
                if (!geometry.isEmpty()) {
                    if (xml.attribute("animation").contains("low-pass")) {
                        Mlt::Filter *blur = new Mlt::Filter(*m_mltProfile, "boxblur");
                        if (blur && blur->is_valid())
                            producer->attach(*blur);
                    }
                    filter->set("transition.geometry", geometry.toUtf8().data());
                    filter->set("transition.cycle", cycle);
                    producer->attach(*filter);
                }
            }
        }
        if (xml.attribute("fade") == "1") {
            // user wants a fade effect to slideshow
            Mlt::Filter *filter = new Mlt::Filter(*m_mltProfile, "luma");
            if (filter && filter->is_valid()) {
                if (ttl) filter->set("cycle", ttl);
                if (xml.hasAttribute("luma_duration") && !xml.attribute("luma_duration").isEmpty()) filter->set("duration", xml.attribute("luma_duration").toInt());
                if (xml.hasAttribute("luma_file") && !xml.attribute("luma_file").isEmpty()) {
                    filter->set("luma.resource", xml.attribute("luma_file").toUtf8().constData());
                    if (xml.hasAttribute("softness")) {
                        int soft = xml.attribute("softness").toInt();
                        filter->set("luma.softness", (double) soft / 100.0);
                    }
                }
                producer->attach(*filter);
            }
        }
        if (xml.attribute("crop") == "1") {
            // user wants to center crop the slides
            Mlt::Filter *filter = new Mlt::Filter(*m_mltProfile, "crop");
            if (filter && filter->is_valid()) {
                filter->set("center", 1);
                producer->attach(*filter);
            }
        }
    }

    if (producer->get_double("meta.media.frame_rate_den") > 0) {
        filePropertyMap["fps"] = locale.toString(producer->get_double("meta.media.frame_rate_num") / producer->get_double("meta.media.frame_rate_den"));
    } else filePropertyMap["fps"] = producer->get("source_fps");

    if (frame && frame->is_valid()) {
        filePropertyMap["frame_size"] = QString::number(frame->get_int("width")) + 'x' + QString::number(frame->get_int("height"));
        filePropertyMap["frequency"] = QString::number(frame->get_int("frequency"));
        filePropertyMap["channels"] = QString::number(frame->get_int("channels"));
        filePropertyMap["aspect_ratio"] = frame->get("aspect_ratio");

        if (frame->get_int("test_image") == 0) {
            if (url.path().endsWith(".mlt") || url.path().endsWith(".westley") || url.path().endsWith(".kdenlive")) {
                filePropertyMap["type"] = "playlist";
                metadataPropertyMap["comment"] = QString::fromUtf8(producer->get("title"));
            } else if (frame->get_int("test_audio") == 0)
                filePropertyMap["type"] = "av";
            else
                filePropertyMap["type"] = "video";

            int variance;
            mlt_image_format format = mlt_image_rgb24a;
            int frame_width = width;
            int frame_height = imageHeight;
            QPixmap pix;
            do {
                variance = 100;
                uint8_t *data = frame->get_image(format, frame_width, frame_height, 0);
                QImage image((uchar *)data, frame_width, frame_height, QImage::Format_ARGB32_Premultiplied);

                if (!image.isNull()) {
                    if (frame_width > (2 * width)) {
                        // there was a scaling problem, do it manually
                        QImage scaled = image.scaled(width, imageHeight);
                        pix = QPixmap::fromImage(scaled.rgbSwapped());
                    } else pix = QPixmap::fromImage(image.rgbSwapped());
                    variance = KThumb::imageVariance(image);
                } else
                    pix.fill(Qt::black);

                if (frameNumber == 0 && variance< 6) {
                    // Thumbnail is not interesting (for example all black, seek to fetch better thumb
                    frameNumber = 100;
                    producer->seek(frameNumber);
                    delete frame;
                    frame = producer->get_frame();
                    variance = -1;
                }
            } while (variance == -1);
            emit replyGetImage(clipId, pix);

        } else if (frame->get_int("test_audio") == 0) {
            QPixmap pixmap = KIcon("audio-x-generic").pixmap(QSize(width, imageHeight));
            emit replyGetImage(clipId, pixmap);
            filePropertyMap["type"] = "audio";
        }
    }
    delete frame;
    // Retrieve audio / video codec name

    // If there is a
    char property[200];
    if (producer->get_int("video_index") > -1) {
        /*if (context->duration == AV_NOPTS_VALUE) {
        kDebug() << " / / / / / / / /ERROR / / / CLIP HAS UNKNOWN DURATION";
            emit removeInvalidClip(clipId);
            delete producer;
            return;
        }*/
        // Get the video_index
        int default_video = producer->get_int("video_index");
        int video_max = 0;
        int default_audio = producer->get_int("audio_index");
        int audio_max = 0;

        // Find maximum stream index values
        for (int ix = 0; ix < producer->get_int("meta.media.nb_streams"); ix++) {
            snprintf(property, sizeof(property), "meta.media.%d.stream.type", ix);
            QString type = producer->get(property);
            if (type == "video")
                video_max = ix;
            else if (type == "audio")
                audio_max = ix;
        }
        filePropertyMap["default_video"] = QString::number(default_video);
        filePropertyMap["video_max"] = QString::number(video_max);
        filePropertyMap["default_audio"] = QString::number(default_audio);
        filePropertyMap["audio_max"] = QString::number(audio_max);

        snprintf(property, sizeof(property), "meta.media.%d.codec.long_name", default_video);
        if (producer->get(property)) {
            filePropertyMap["videocodec"] = producer->get(property);
        } else {
            snprintf(property, sizeof(property), "meta.media.%d.codec.name", default_video);
            if (producer->get(property))
                filePropertyMap["videocodec"] = producer->get(property);
        }
        QString query;
        query = QString("meta.media.%1.codec.pix_fmt").arg(default_video);
        filePropertyMap["pix_fmt"] = producer->get(query.toUtf8().constData());
        filePropertyMap["colorspace"] = producer->get("meta.media.colorspace");

    } else kDebug() << " / / / / /WARNING, VIDEO CONTEXT IS NULL!!!!!!!!!!!!!!";
    if (producer->get_int("audio_index") > -1) {
        // Get the audio_index
        int index = producer->get_int("audio_index");

        snprintf(property, sizeof(property), "meta.media.%d.codec.long_name", index);
        if (producer->get(property)) {
            filePropertyMap["audiocodec"] = producer->get(property);
        } else {
            snprintf(property, sizeof(property), "meta.media.%d.codec.name", index);
            if (producer->get(property))
                filePropertyMap["audiocodec"] = producer->get(property);
        }
    }

    // metadata
    Mlt::Properties metadata;
    metadata.pass_values(*producer, "meta.attr.");
    int count = metadata.count();
    for (int i = 0; i < count; i ++) {
        QString name = metadata.get_name(i);
        QString value = QString::fromUtf8(metadata.get(i));
        if (name.endsWith("markup") && !value.isEmpty())
            metadataPropertyMap[ name.section('.', 0, -2)] = value;
    }
    producer->seek(0);
    if (replaceProducer) emit blockClipMonitor(clipId);
    emit replyGetFileProperties(clipId, producer, filePropertyMap, metadataPropertyMap, replaceProducer, selectClip);
    // FIXME: should delete this to avoid a leak...
    //delete producer;
}


#if 0
/** Create the producer from the MLT XML QDomDocument */
void Render::initSceneList()
{
    kDebug() << "--------  INIT SCENE LIST ------_";
    QDomDocument doc;
    QDomElement mlt = doc.createElement("mlt");
    doc.appendChild(mlt);
    QDomElement prod = doc.createElement("producer");
    prod.setAttribute("resource", "colour");
    prod.setAttribute("colour", "red");
    prod.setAttribute("id", "black");
    prod.setAttribute("in", "0");
    prod.setAttribute("out", "0");

    QDomElement tractor = doc.createElement("tractor");
    QDomElement multitrack = doc.createElement("multitrack");

    QDomElement playlist1 = doc.createElement("playlist");
    playlist1.appendChild(prod);
    multitrack.appendChild(playlist1);
    QDomElement playlist2 = doc.createElement("playlist");
    multitrack.appendChild(playlist2);
    QDomElement playlist3 = doc.createElement("playlist");
    multitrack.appendChild(playlist3);
    QDomElement playlist4 = doc.createElement("playlist");
    multitrack.appendChild(playlist4);
    QDomElement playlist5 = doc.createElement("playlist");
    multitrack.appendChild(playlist5);
    tractor.appendChild(multitrack);
    mlt.appendChild(tractor);
    // kDebug()<<doc.toString();
    /*
       QString tmp = QString("<mlt><producer resource=\"colour\" colour=\"red\" id=\"red\" /><tractor><multitrack><playlist></playlist><playlist></playlist><playlist /><playlist /><playlist></playlist></multitrack></tractor></mlt>");*/
    setSceneList(doc, 0);
}
#endif

int Render::setProducer(Mlt::Producer *producer, int position)
{
    QMutexLocker locker(&m_mutex);
    if (m_winid == -1) return -1;
    if (m_mltConsumer) {
        if (!m_mltConsumer->is_stopped()) {
            m_mltConsumer->stop();
        }
        m_mltConsumer->set("refresh", 0);
    }
    else {
        return -1;
    }

    m_isBlocked = true;
    if (m_mltProducer) {
        m_mltProducer->set_speed(0);
        delete m_mltProducer;
        m_mltProducer = NULL;
        emit stopped();
    }
    blockSignals(true);
    if (producer && producer->is_valid()) {
        m_mltProducer = new Mlt::Producer(producer->get_producer());
    } else m_mltProducer = m_blackClip->cut(0, 50);

    if (!m_mltProducer || !m_mltProducer->is_valid()) {
        kDebug() << " WARNING - - - - -INVALID PLAYLIST: ";
        return -1;
    }
    int volume = KdenliveSettings::volume();
    m_mltProducer->set("meta.volume", (double)volume / 100);
    m_fps = m_mltProducer->get_fps();
    blockSignals(false);
    int error = connectPlaylist();

    if (position != -1) {
        m_mltProducer->seek(position);
        emit rendererPosition(position);
    } else emit rendererPosition((int) m_mltProducer->position());
    m_isBlocked = false;
    return error;
}

int Render::setSceneList(QDomDocument list, int position)
{
    return setSceneList(list.toString(), position);
}

int Render::setSceneList(QString playlist, int position)
{
    if (m_winid == -1) return -1;
    m_isBlocked = true;
    int error = 0;

    //kDebug() << "//////  RENDER, SET SCENE LIST:\n" << playlist <<"\n..........:::.";

    // Remove previous profile info
    QDomDocument doc;
    doc.setContent(playlist);
    QDomElement profile = doc.documentElement().firstChildElement("profile");
    doc.documentElement().removeChild(profile);
    playlist = doc.toString();

    if (m_mltConsumer) {
        if (!m_mltConsumer->is_stopped()) {
            m_mltConsumer->stop();
        }
        m_mltConsumer->set("refresh", 0);
    } else {
        kWarning() << "///////  ERROR, TRYING TO USE NULL MLT CONSUMER";
        error = -1;
    }

    if (m_mltProducer) {
        m_mltProducer->set_speed(0);
        //if (KdenliveSettings::osdtimecode() && m_osdInfo) m_mltProducer->detach(*m_osdInfo);

        /*Mlt::Service service(m_mltProducer->parent().get_service());
        mlt_service_lock(service.get_service());

        if (service.type() == tractor_type) {
            Mlt::Tractor tractor(service);
            Mlt::Field *field = tractor.field();
            mlt_service nextservice = mlt_service_get_producer(service.get_service());
            mlt_service nextservicetodisconnect;
            mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
            QString mlt_type = mlt_properties_get(properties, "mlt_type");
            QString resource = mlt_properties_get(properties, "mlt_service");
            // Delete all transitions
            while (mlt_type == "transition") {
                nextservicetodisconnect = nextservice;
                nextservice = mlt_service_producer(nextservice);
                mlt_field_disconnect_service(field->get_field(), nextservicetodisconnect);
                if (nextservice == NULL) break;
                properties = MLT_SERVICE_PROPERTIES(nextservice);
                mlt_type = mlt_properties_get(properties, "mlt_type");
                resource = mlt_properties_get(properties, "mlt_service");
            }


            for (int trackNb = tractor.count() - 1; trackNb >= 0; --trackNb) {
                Mlt::Producer trackProducer(tractor.track(trackNb));
                Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
                if (trackPlaylist.type() == playlist_type) trackPlaylist.clear();
            }
            delete field;
        }
        mlt_service_unlock(service.get_service());*/

        qDeleteAll(m_slowmotionProducers.values());
        m_slowmotionProducers.clear();

        delete m_mltProducer;
        m_mltProducer = NULL;
        emit stopped();
    }

    blockSignals(true);
    // WARNING: disabled because it caused crashes (see Kdenlive bug #2205 and #2206) - jbm
    /*if (KdenliveSettings::projectloading_avformatnovalidate())
        playlist.replace(">avformat</property>", ">avformat-novalidate</property>");
    else
        playlist.replace(">avformat-novalidate</property>", ">avformat</property>");*/
    m_mltProducer = new Mlt::Producer(*m_mltProfile, "xml-string", playlist.toUtf8().constData());
    if (!m_mltProducer || !m_mltProducer->is_valid()) {
        kDebug() << " WARNING - - - - -INVALID PLAYLIST: " << playlist.toUtf8().constData();
        m_mltProducer = m_blackClip->cut(0, 50);
        error = -1;
    }
    int volume = KdenliveSettings::volume();
    m_mltProducer->set("meta.volume", (double)volume / 100);
    m_mltProducer->optimise();

    /*if (KdenliveSettings::osdtimecode()) {
    // Attach filter for on screen display of timecode
    delete m_osdInfo;
    QString attr = "attr_check";
    mlt_filter filter = mlt_factory_filter( "data_feed", (char*) attr.ascii() );
    mlt_properties_set_int( MLT_FILTER_PROPERTIES( filter ), "_loader", 1 );
    mlt_producer_attach( m_mltProducer->get_producer(), filter );
    mlt_filter_close( filter );

      m_osdInfo = new Mlt::Filter("data_show");
    m_osdInfo->set("resource", m_osdProfile.toUtf8().constData());
    mlt_properties properties = MLT_PRODUCER_PROPERTIES(m_mltProducer->get_producer());
    mlt_properties_set_int( properties, "meta.attr.timecode", 1);
    mlt_properties_set( properties, "meta.attr.timecode.markup", "#timecode#");
    m_osdInfo->set("dynamic", "1");

      if (m_mltProducer->attach(*m_osdInfo) == 1) kDebug()<<"////// error attaching filter";
    } else {
    m_osdInfo->set("dynamic", "0");
    }*/

    m_fps = m_mltProducer->get_fps();
    if (position != 0) {
        // Seek to correct place after opening project.
        m_mltProducer->seek(position);
    }

    kDebug() << "// NEW SCENE LIST DURATION SET TO: " << m_mltProducer->get_playtime();
    if (error == 0) error = connectPlaylist();
    else connectPlaylist();
    fillSlowMotionProducers();

    m_isBlocked = false;
    blockSignals(false);

    return error;
    //kDebug()<<"// SETSCN LST, POS: "<<position;
    //if (position != 0) emit rendererPosition(position);
}

const QString Render::sceneList()
{
    QString playlist;
    Mlt::Profile profile((mlt_profile) 0);
    Mlt::Consumer xmlConsumer(profile, "xml:kdenlive_playlist");
    m_mltProducer->optimise();
    xmlConsumer.set("terminate_on_pause", 1);
    Mlt::Producer prod(m_mltProducer->get_producer());
    bool split = m_isSplitView;
    if (split) slotSplitView(false);
    xmlConsumer.connect(prod);
    xmlConsumer.start();
    while (!xmlConsumer.is_stopped()) {}
    playlist = QString::fromUtf8(xmlConsumer.get("kdenlive_playlist"));
    if (split) slotSplitView(true);
    return playlist;
}

bool Render::saveSceneList(QString path, QDomElement kdenliveData)
{
    QFile file(path);
    QDomDocument doc;
    doc.setContent(sceneList(), false);
    if (!kdenliveData.isNull()) {
        // add Kdenlive specific tags
        QDomNode mlt = doc.elementsByTagName("mlt").at(0);
        mlt.appendChild(doc.importNode(kdenliveData, true));
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        kWarning() << "//////  ERROR writing to file: " << path;
        return false;
    }
    file.write(doc.toString().toUtf8());
    if (file.error() != QFile::NoError) {
        file.close();
        return false;
    }
    file.close();
    return true;
}

void Render::saveZone(KUrl url, QString desc, QPoint zone)
{
    Mlt::Consumer xmlConsumer(*m_mltProfile, ("xml:" + url.path()).toUtf8().constData());
    m_mltProducer->optimise();
    xmlConsumer.set("terminate_on_pause", 1);
    if (m_name == "clip") {
        Mlt::Producer *prod = m_mltProducer->cut(zone.x(), zone.y());
        Mlt::Playlist list;
        list.insert_at(0, prod, 0);
        delete prod;
        list.set("title", desc.toUtf8().constData());
        xmlConsumer.connect(list);

    } else {
        //TODO: not working yet, save zone from timeline
        Mlt::Producer *p1 = new Mlt::Producer(m_mltProducer->get_producer());
        /* Mlt::Service service(p1->parent().get_service());
         if (service.type() != tractor_type) kWarning() << "// TRACTOR PROBLEM";*/

        //Mlt::Producer *prod = p1->cut(zone.x(), zone.y());
        //prod->set("title", desc.toUtf8().constData());
        xmlConsumer.connect(*p1); //list);
    }

    xmlConsumer.start();
}

double Render::fps() const
{
    return m_fps;
}

int Render::connectPlaylist()
{
    if (!m_mltConsumer) return -1;
    //m_mltConsumer->set("refresh", "0");
    m_mltConsumer->connect(*m_mltProducer);
    m_mltProducer->set_speed(0);
    if (m_mltConsumer->start() == -1) {
        // ARGH CONSUMER BROKEN!!!!
        KMessageBox::error(qApp->activeWindow(), i18n("Could not create the video preview window.\nThere is something wrong with your Kdenlive install or your driver settings, please fix it."));
        delete m_mltConsumer;
        m_mltConsumer = NULL;
        return -1;
    }
    emit durationChanged(m_mltProducer->get_playtime());
    return 0;
    //refresh();
}

void Render::refreshDisplay()
{

    if (!m_mltProducer) return;
    //m_mltConsumer->set("refresh", 0);

    //mlt_properties properties = MLT_PRODUCER_PROPERTIES(m_mltProducer->get_producer());
    /*if (KdenliveSettings::osdtimecode()) {
        mlt_properties_set_int( properties, "meta.attr.timecode", 1);
        mlt_properties_set( properties, "meta.attr.timecode.markup", "#timecode#");
        m_osdInfo->set("dynamic", "1");
        m_mltProducer->attach(*m_osdInfo);
    }
    else {
        m_mltProducer->detach(*m_osdInfo);
        m_osdInfo->set("dynamic", "0");
    }*/
    refresh();
}

int Render::volume() const
{
    if (!m_mltConsumer || !m_mltProducer) return -1;
    return ((int) 100 * m_mltProducer->get_double("meta.volume"));
}

void Render::slotSetVolume(int volume)
{
    if (!m_mltConsumer || !m_mltProducer) return;
    m_mltProducer->set("meta.volume", (double)volume / 100.0);
    return;
    /*osdTimer->stop();
    m_mltConsumer->set("refresh", 0);
    // Attach filter for on screen display of timecode
    mlt_properties properties = MLT_PRODUCER_PROPERTIES(m_mltProducer->get_producer());
    mlt_properties_set_double( properties, "meta.volume", volume );
    mlt_properties_set_int( properties, "meta.attr.osdvolume", 1);
    mlt_properties_set( properties, "meta.attr.osdvolume.markup", i18n("Volume: ") + QString::number(volume * 100));

    if (!KdenliveSettings::osdtimecode()) {
    m_mltProducer->detach(*m_osdInfo);
    mlt_properties_set_int( properties, "meta.attr.timecode", 0);
     if (m_mltProducer->attach(*m_osdInfo) == 1) kDebug()<<"////// error attaching filter";
    }*/
    refresh();
    //m_osdTimer->setSingleShot(2500);
}

void Render::slotOsdTimeout()
{
    mlt_properties properties = MLT_PRODUCER_PROPERTIES(m_mltProducer->get_producer());
    mlt_properties_set_int(properties, "meta.attr.osdvolume", 0);
    mlt_properties_set(properties, "meta.attr.osdvolume.markup", NULL);
    //if (!KdenliveSettings::osdtimecode()) m_mltProducer->detach(*m_osdInfo);
    refresh();
}

void Render::start()
{
    if (m_winid == -1) {
        kDebug() << "-----  BROKEN MONITOR: " << m_name << ", RESTART";
        return;
    }
    if (m_mltConsumer && m_mltConsumer->is_stopped()) {
        if (m_mltConsumer->start() == -1) {
            //KMessageBox::error(qApp->activeWindow(), i18n("Could not create the video preview window.\nThere is something wrong with your Kdenlive install or your driver settings, please fix it."));
            kDebug(QtWarningMsg) << "/ / / / CANNOT START MONITOR";
        } else {
            m_isBlocked = false;
            refresh();
        }
    }
    m_isBlocked = false;
}

void Render::stop()
{
    if (m_mltProducer == NULL) return;
    if (m_mltConsumer && !m_mltConsumer->is_stopped()) {
        //kDebug() << "/////////////   RENDER STOPPED: " << m_name;
        //m_mltConsumer->set("refresh", 0);
        m_mltConsumer->stop();
        // delete m_mltConsumer;
        // m_mltConsumer = NULL;
    }
    m_isBlocked = true;

    if (m_mltProducer) {
        if (m_isZoneMode) resetZoneMode();
        m_mltProducer->set_speed(0.0);
        //m_mltProducer->set("out", m_mltProducer->get_length() - 1);
        //kDebug() << m_mltProducer->get_length();
    }
}

void Render::stop(const GenTime & startTime)
{
    if (m_mltProducer) {
        if (m_isZoneMode) resetZoneMode();
        m_mltProducer->set_speed(0.0);
        m_mltProducer->seek((int) startTime.frames(m_fps));
    }
    m_mltConsumer->purge();
}

void Render::pause()
{
    if (!m_mltProducer || !m_mltConsumer)
        return;
    if (m_mltProducer->get_speed() == 0.0) return;
    if (m_isZoneMode) resetZoneMode();
    m_isBlocked = true;
    m_mltConsumer->set("refresh", 0);
    m_mltProducer->set_speed(0.0);
    /*
    The 2 lines below create a flicker loop
    emit rendererPosition(m_framePosition);
    m_mltProducer->seek(m_framePosition);*/
    m_mltConsumer->purge();
}

void Render::switchPlay(bool play)
{
    if (!m_mltProducer || !m_mltConsumer)
        return;
    if (m_isZoneMode) resetZoneMode();
    if (play && m_mltProducer->get_speed() == 0.0) {
        m_isBlocked = false;
        if (m_name == "clip" && m_framePosition == (int) m_mltProducer->get_out()) m_mltProducer->seek(0);
        m_mltProducer->set_speed(1.0);
        m_mltConsumer->set("refresh", 1);
    } else if (!play) {
        m_isBlocked = true;
        m_mltConsumer->set("refresh", 0);
        m_mltProducer->set_speed(0.0);
        //emit rendererPosition(m_framePosition);
        m_mltProducer->seek(m_framePosition);
        //m_mltConsumer->purge();
    }
}

void Render::play(double speed)
{
    if (!m_mltProducer)
        return;
    // if (speed == 0.0) m_mltProducer->set("out", m_mltProducer->get_length() - 1);
    m_isBlocked = false;
    m_mltProducer->set_speed(speed);
    /*if (speed == 0.0) {
    m_mltProducer->seek((int) m_framePosition + 1);
        m_mltConsumer->purge();
    }*/
    refresh();
}

void Render::play(const GenTime & startTime)
{
    if (!m_mltProducer || !m_mltConsumer)
        return;
    m_isBlocked = false;
    m_mltProducer->seek((int)(startTime.frames(m_fps)));
    m_mltProducer->set_speed(1.0);
    m_mltConsumer->set("refresh", 1);
}

void Render::loopZone(const GenTime & startTime, const GenTime & stopTime)
{
    if (!m_mltProducer || !m_mltConsumer)
        return;
    //m_mltProducer->set("eof", "loop");
    m_isLoopMode = true;
    m_loopStart = startTime;
    playZone(startTime, stopTime);
}

void Render::playZone(const GenTime & startTime, const GenTime & stopTime)
{
    if (!m_mltProducer || !m_mltConsumer)
        return;
    m_isBlocked = false;
    if (!m_isZoneMode) m_originalOut = m_mltProducer->get_playtime() - 1;
    m_mltProducer->set("out", (int)(stopTime.frames(m_fps)));
    m_mltProducer->seek((int)(startTime.frames(m_fps)));
    m_mltProducer->set_speed(1.0);
    m_mltConsumer->set("refresh", 1);
    m_isZoneMode = true;
}

void Render::resetZoneMode()
{
    if (!m_isZoneMode && !m_isLoopMode) return;
    m_mltProducer->set("out", m_originalOut);
    //m_mltProducer->set("eof", "pause");
    m_isZoneMode = false;
    m_isLoopMode = false;
}

void Render::seekToFrame(int pos)
{
    //kDebug()<<" *********  RENDER SEEK TO POS";
    if (!m_mltProducer)
        return;
    m_isBlocked = false;
    resetZoneMode();
    m_mltProducer->seek(pos);
    refresh();
}

void Render::seekToFrameDiff(int diff)
{
    //kDebug()<<" *********  RENDER SEEK TO POS";
    if (!m_mltProducer)
        return;
    m_isBlocked = false;
    resetZoneMode();
    m_mltProducer->seek(m_mltProducer->position() + diff);
    refresh();
}

void Render::doRefresh()
{
    // Use a Timer so that we don't refresh too much
    if (!m_isBlocked && m_mltConsumer) m_mltConsumer->set("refresh", 1);
}

void Render::refresh()
{
    if (!m_mltProducer || m_isBlocked)
        return;
    if (m_mltConsumer) {
        m_mltConsumer->set("refresh", 1);
    }
}

void Render::setDropFrames(bool show)
{
    if (m_mltConsumer) {
        int dropFrames = KdenliveSettings::mltthreads();
        if (show == false) dropFrames = -dropFrames;
        m_mltConsumer->stop();
        if (m_winid == 0)
            m_mltConsumer->set("real_time", dropFrames);
        else
            m_mltConsumer->set("play.real_time", dropFrames);

        if (m_mltConsumer->start() == -1) {
            kDebug(QtWarningMsg) << "ERROR, Cannot start monitor";
        }

    }
}

double Render::playSpeed()
{
    if (m_mltProducer) return m_mltProducer->get_speed();
    return 0.0;
}

GenTime Render::seekPosition() const
{
    if (m_mltProducer) return GenTime((int) m_mltProducer->position(), m_fps);
    else return GenTime();
}

int Render::seekFramePosition() const
{
    if (m_mltProducer) return (int) m_mltProducer->position();
    return 0;
}

const QString & Render::rendererName() const
{
    return m_name;
}

void Render::emitFrameUpdated(Mlt::Frame& frame)
{
    mlt_image_format format = mlt_image_rgb24a;
    int width = 0;
    int height = 0;
    const uchar* image = frame.get_image(format, width, height);
    QImage qimage(width, height, QImage::Format_ARGB32);
    memcpy(qimage.bits(), image, width * height * 4);
    emit frameUpdated(qimage.rgbSwapped());
}

void Render::emitFrameNumber(double position)
{
    m_framePosition = position;
    emit rendererPosition((int) position);
}

void Render::emitConsumerStopped()
{
    // This is used to know when the playing stopped
    if (m_mltProducer) {
        double pos = m_mltProducer->position();
        if (m_isLoopMode) play(m_loopStart);
        else if (m_isZoneMode) resetZoneMode();
        emit rendererStopped((int) pos);
    }
}

void Render::exportFileToFirewire(QString /*srcFileName*/, int /*port*/, GenTime /*startTime*/, GenTime /*endTime*/)
{
    KMessageBox::sorry(0, i18n("Firewire is not enabled on your system.\n Please install Libiec61883 and recompile Kdenlive"));
}

void Render::exportCurrentFrame(KUrl url, bool /*notify*/)
{
    if (!m_mltProducer) {
        KMessageBox::sorry(qApp->activeWindow(), i18n("There is no clip, cannot extract frame."));
        return;
    }

    //int height = 1080;//KdenliveSettings::defaultheight();
    //int width = 1940; //KdenliveSettings::displaywidth();
    //TODO: rewrite
    QPixmap pix; // = KThumb::getFrame(m_mltProducer, -1, width, height);
    /*
       QPixmap pix(width, height);
       Mlt::Filter m_convert(*m_mltProfile, "avcolour_space");
       m_convert.set("forced", mlt_image_rgb24a);
       m_mltProducer->attach(m_convert);
       Mlt::Frame * frame = m_mltProducer->get_frame();
       m_mltProducer->detach(m_convert);
       if (frame) {
           pix = frameThumbnail(frame, width, height);
           delete frame;
       }*/
    pix.save(url.path(), "PNG");
    //if (notify) QApplication::postEvent(qApp->activeWindow(), new UrlEvent(url, 10003));
}


void Render::showFrame(Mlt::Frame& frame)
{
    m_framePosition = qMax(frame.get_int("_position"), 0);
    emit rendererPosition((int) m_framePosition);
    mlt_image_format format = mlt_image_rgb24a;
    int width = 0;
    int height = 0;
    const uchar* image = frame.get_image(format, width, height);
    QImage qimage(width, height, QImage::Format_ARGB32_Premultiplied);
    memcpy(qimage.scanLine(0), image, width * height * 4);
    emit showImageSignal(qimage);
    if (analyseAudio) showAudio(frame);
    if (sendFrameForAnalysis && frame.get_frame()->convert_image) {
        emit frameUpdated(qimage.rgbSwapped());
    }
}

void Render::showAudio(Mlt::Frame& frame)
{
    if (!frame.is_valid() || frame.get_int("test_audio") != 0) {
        return;
    }
    mlt_audio_format audio_format = mlt_audio_s16;
    int freq = 0;
    int num_channels = 0;
    int samples = 0;
    int16_t* data = (int16_t*)frame.get_audio(audio_format, freq, num_channels, samples);

    if (!data) {
        return;
    }

    // Data format: [ c00 c10 c01 c11 c02 c12 c03 c13 ... c0{samples-1} c1{samples-1} for 2 channels.
    // So the vector is of size samples*channels.
    QVector<int16_t> sampleVector(samples*num_channels);
    memcpy(sampleVector.data(), data, samples*num_channels*sizeof(int16_t));

    if (samples > 0) {
        emit audioSamplesSignal(sampleVector, freq, num_channels, samples);
    }
}

/*
 * MLT playlist direct manipulation.
 */

void Render::mltCheckLength(Mlt::Tractor *tractor)
{
    //kDebug()<<"checking track length: "<<track<<"..........";

    int trackNb = tractor->count();
    int duration = 0;
    int trackDuration;
    if (trackNb == 1) {
        Mlt::Producer trackProducer(tractor->track(0));
        duration = trackProducer.get_playtime() - 1;
        m_mltProducer->set("out", duration);
        emit durationChanged(duration);
        return;
    }
    while (trackNb > 1) {
        Mlt::Producer trackProducer(tractor->track(trackNb - 1));
        trackDuration = trackProducer.get_playtime() - 1;
        // kDebug() << " / / /DURATON FOR TRACK " << trackNb - 1 << " = " << trackDuration;
        if (trackDuration > duration) duration = trackDuration;
        trackNb--;
    }

    Mlt::Producer blackTrackProducer(tractor->track(0));

    if (blackTrackProducer.get_playtime() - 1 != duration) {
        Mlt::Playlist blackTrackPlaylist((mlt_playlist) blackTrackProducer.get_service());
        Mlt::Producer *blackclip = blackTrackPlaylist.get_clip(0);
        if (blackclip && blackclip->is_blank()) {
            delete blackclip;
            blackclip = NULL;
        }

        if (blackclip == NULL || blackTrackPlaylist.count() != 1) {
            blackTrackPlaylist.clear();
            m_blackClip->set("length", duration + 1);
            m_blackClip->set("out", duration);
            blackclip = m_blackClip->cut(0, duration);
            blackTrackPlaylist.insert_at(0, blackclip, 1);
        } else {
            if (duration > blackclip->parent().get_length()) {
                blackclip->parent().set("length", duration + 1);
                blackclip->parent().set("out", duration);
                blackclip->set("length", duration + 1);
            }
            blackTrackPlaylist.resize_clip(0, 0, duration);
        }

        delete blackclip;
        m_mltProducer->set("out", duration);
        emit durationChanged(duration);
    }
}

Mlt::Producer *Render::checkSlowMotionProducer(Mlt::Producer *prod, QDomElement element)
{
    if (element.attribute("speed", "1.0").toDouble() == 1.0 && element.attribute("strobe", "1").toInt() == 1) return prod;
    QLocale locale;
    // We want a slowmotion producer
    double speed = element.attribute("speed", "1.0").toDouble();
    int strobe = element.attribute("strobe", "1").toInt();
    QString url = QString::fromUtf8(prod->get("resource"));
    url.append('?' + locale.toString(speed));
    if (strobe > 1) url.append("&strobe=" + QString::number(strobe));
    Mlt::Producer *slowprod = m_slowmotionProducers.value(url);
    if (!slowprod || slowprod->get_producer() == NULL) {
        slowprod = new Mlt::Producer(*m_mltProfile, 0, ("framebuffer:" + url).toUtf8().constData());
        if (strobe > 1) slowprod->set("strobe", strobe);
        QString id = prod->parent().get("id");
        if (id.contains('_')) id = id.section('_', 0, 0);
        QString producerid = "slowmotion:" + id + ':' + locale.toString(speed);
        if (strobe > 1) producerid.append(':' + QString::number(strobe));
        slowprod->set("id", producerid.toUtf8().constData());
        m_slowmotionProducers.insert(url, slowprod);
    }
    return slowprod;
}

int Render::mltInsertClip(ItemInfo info, QDomElement element, Mlt::Producer *prod, bool overwrite, bool push)
{
    if (m_mltProducer == NULL) {
        kDebug() << "PLAYLIST NOT INITIALISED //////";
        return -1;
    }
    if (prod == NULL) {
        kDebug() << "Cannot insert clip without producer //////";
        return -1;
    }
    Mlt::Producer parentProd(m_mltProducer->parent());
    if (parentProd.get_producer() == NULL) {
        kDebug() << "PLAYLIST BROKEN, CANNOT INSERT CLIP //////";
        return -1;
    }

    Mlt::Service service(parentProd.get_service());
    if (service.type() != tractor_type) {
        kWarning() << "// TRACTOR PROBLEM";
        return -1;
    }
    Mlt::Tractor tractor(service);
    if (info.track > tractor.count() - 1) {
        kDebug() << "ERROR TRYING TO INSERT CLIP ON TRACK " << info.track << ", at POS: " << info.startPos.frames(25);
        return -1;
    }
    mlt_service_lock(service.get_service());
    Mlt::Producer trackProducer(tractor.track(info.track));
    int trackDuration = trackProducer.get_playtime() - 1;
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
    //kDebug()<<"/// INSERT cLIP: "<<info.cropStart.frames(m_fps)<<", "<<info.startPos.frames(m_fps)<<"-"<<info.endPos.frames(m_fps);
    prod = checkSlowMotionProducer(prod, element);
    if (prod == NULL || !prod->is_valid()) {
        mlt_service_unlock(service.get_service());
        return -1;
    }

    int cutPos = (int) info.cropStart.frames(m_fps);
    if (cutPos < 0) cutPos = 0;
    int insertPos = (int) info.startPos.frames(m_fps);
    int cutDuration = (int)(info.endPos - info.startPos).frames(m_fps) - 1;
    Mlt::Producer *clip = prod->cut(cutPos, cutDuration + cutPos);
    if (overwrite && (insertPos < trackDuration)) {
        // Replace zone with blanks
        //trackPlaylist.split_at(insertPos, true);
        trackPlaylist.remove_region(insertPos, cutDuration + 1);
        int clipIndex = trackPlaylist.get_clip_index_at(insertPos);
        trackPlaylist.insert_blank(clipIndex, cutDuration);
    } else if (push) {
        trackPlaylist.split_at(insertPos, true);
        int clipIndex = trackPlaylist.get_clip_index_at(insertPos);
        trackPlaylist.insert_blank(clipIndex, cutDuration);
    }
    int newIndex = trackPlaylist.insert_at(insertPos, clip, 1);
    delete clip;
    /*if (QString(prod->get("transparency")).toInt() == 1)
        mltAddClipTransparency(info, info.track - 1, QString(prod->get("id")).toInt());*/

    if (info.track != 0 && (newIndex + 1 == trackPlaylist.count())) mltCheckLength(&tractor);
    mlt_service_unlock(service.get_service());
    /*tractor.multitrack()->refresh();
    tractor.refresh();*/
    return 0;
}


void Render::mltCutClip(int track, GenTime position)
{
    m_isBlocked = true;

    Mlt::Service service(m_mltProducer->parent().get_service());
    if (service.type() != tractor_type) {
        kWarning() << "// TRACTOR PROBLEM";
        return;
    }

    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());


    /* // Display playlist info
    kDebug()<<"////////////  BEFORE";
    for (int i = 0; i < trackPlaylist.count(); i++) {
    int blankStart = trackPlaylist.clip_start(i);
    int blankDuration = trackPlaylist.clip_length(i) - 1;
    QString blk;
    if (trackPlaylist.is_blank(i)) blk = "(blank)";
    kDebug()<<"CLIP "<<i<<": ("<<blankStart<<'x'<<blankStart + blankDuration<<")"<<blk;
    }*/

    int cutPos = (int) position.frames(m_fps);

    int clipIndex = trackPlaylist.get_clip_index_at(cutPos);
    if (trackPlaylist.is_blank(clipIndex)) {
        kDebug() << "// WARNING, TRYING TO CUT A BLANK";
        m_isBlocked = false;
        return;
    }
    mlt_service_lock(service.get_service());
    int clipStart = trackPlaylist.clip_start(clipIndex);
    trackPlaylist.split(clipIndex, cutPos - clipStart - 1);
    mlt_service_unlock(service.get_service());

    // duplicate effects
    Mlt::Producer *original = trackPlaylist.get_clip_at(clipStart);
    Mlt::Producer *clip = trackPlaylist.get_clip_at(cutPos);

    if (original == NULL || clip == NULL) {
        kDebug() << "// ERROR GRABBING CLIP AFTER SPLIT";
    }
    Mlt::Service clipService(original->get_service());
    Mlt::Service dupService(clip->get_service());
    delete original;
    delete clip;
    int ct = 0;
    Mlt::Filter *filter = clipService.filter(ct);
    while (filter) {
        // Only duplicate Kdenlive filters, and skip the fade in effects
        if (filter->is_valid() && strcmp(filter->get("kdenlive_id"), "") && strcmp(filter->get("kdenlive_id"), "fadein") && strcmp(filter->get("kdenlive_id"), "fade_from_black")) {
            // looks like there is no easy way to duplicate a filter,
            // so we will create a new one and duplicate its properties
            Mlt::Filter *dup = new Mlt::Filter(*m_mltProfile, filter->get("mlt_service"));
            if (dup && dup->is_valid()) {
                Mlt::Properties entries(filter->get_properties());
                for (int i = 0; i < entries.count(); i++) {
                    dup->set(entries.get_name(i), entries.get(i));
                }
                dupService.attach(*dup);
            }
        }
        ct++;
        filter = clipService.filter(ct);
    }

    /* // Display playlist info
    kDebug()<<"////////////  AFTER";
    for (int i = 0; i < trackPlaylist.count(); i++) {
    int blankStart = trackPlaylist.clip_start(i);
    int blankDuration = trackPlaylist.clip_length(i) - 1;
    QString blk;
    if (trackPlaylist.is_blank(i)) blk = "(blank)";
    kDebug()<<"CLIP "<<i<<": ("<<blankStart<<'x'<<blankStart + blankDuration<<")"<<blk;
    }*/

    m_isBlocked = false;
}

bool Render::mltUpdateClip(ItemInfo info, QDomElement element, Mlt::Producer *prod)
{
    // TODO: optimize
    if (prod == NULL) {
        kDebug() << "Cannot update clip with null producer //////";
        return false;
    }
    Mlt::Service service(m_mltProducer->parent().get_service());
    if (service.type() != tractor_type) {
        kWarning() << "// TRACTOR PROBLEM";
        return false;
    }
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(tractor.count() - 1 - info.track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
    int startPos = info.startPos.frames(m_fps);
    int clipIndex = trackPlaylist.get_clip_index_at(startPos);
    if (trackPlaylist.is_blank(clipIndex)) {
        kDebug() << "// WARNING, TRYING TO REMOVE A BLANK: " << startPos;
        return false;
    }
    mlt_service_lock(service.get_service());
    Mlt::Producer *clip = trackPlaylist.get_clip(clipIndex);
    // keep effects
    QList <Mlt::Filter *> filtersList;
    Mlt::Service sourceService(clip->get_service());
    int ct = 0;
    Mlt::Filter *filter = sourceService.filter(ct);
    while (filter) {
        if (filter->get_int("kdenlive_ix") != 0) {
            filtersList.append(filter);
        }
        ct++;
        filter = sourceService.filter(ct);
    }

    trackPlaylist.replace_with_blank(clipIndex);
    delete clip;
    //if (!mltRemoveClip(info.track, info.startPos)) return false;
    prod = checkSlowMotionProducer(prod, element);
    if (prod == NULL || !prod->is_valid()) {
        mlt_service_unlock(service.get_service());
        return false;
    }

    Mlt::Producer *clip2 = prod->cut(info.cropStart.frames(m_fps), (info.cropDuration + info.cropStart).frames(m_fps) - 1);
    trackPlaylist.insert_at(info.startPos.frames(m_fps), clip2, 1);
    delete clip2;


    //if (mltInsertClip(info, element, prod) == -1) return false;
    if (!filtersList.isEmpty()) {
        clipIndex = trackPlaylist.get_clip_index_at(startPos);
        Mlt::Producer *destclip = trackPlaylist.get_clip(clipIndex);
        Mlt::Service destService(destclip->get_service());
        delete destclip;
        for (int i = 0; i < filtersList.count(); i++)
            destService.attach(*(filtersList.at(i)));
    }
    mlt_service_unlock(service.get_service());
    return true;
}


bool Render::mltRemoveClip(int track, GenTime position)
{
    Mlt::Service service(m_mltProducer->parent().get_service());
    if (service.type() != tractor_type) {
        kWarning() << "// TRACTOR PROBLEM";
        return false;
    }
    //service.lock();
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
    int clipIndex = trackPlaylist.get_clip_index_at((int) position.frames(m_fps));

    if (trackPlaylist.is_blank(clipIndex)) {
        kDebug() << "// WARNING, TRYING TO REMOVE A BLANK: " << position.frames(m_fps);
        //mlt_service_unlock(service.get_service());
        return false;
    }
    m_isBlocked = true;
    Mlt::Producer *clip = trackPlaylist.replace_with_blank(clipIndex);
    if (clip) delete clip;
    trackPlaylist.consolidate_blanks(0);

    /* // Display playlist info
    kDebug()<<"////  AFTER";
    for (int i = 0; i < trackPlaylist.count(); i++) {
    int blankStart = trackPlaylist.clip_start(i);
    int blankDuration = trackPlaylist.clip_length(i) - 1;
    QString blk;
    if (trackPlaylist.is_blank(i)) blk = "(blank)";
    kDebug()<<"CLIP "<<i<<": ("<<blankStart<<'x'<<blankStart + blankDuration<<")"<<blk;
    }*/
    //service.unlock();
    if (track != 0 && trackPlaylist.count() <= clipIndex) mltCheckLength(&tractor);
    m_isBlocked = false;
    return true;
}

int Render::mltGetSpaceLength(const GenTime pos, int track, bool fromBlankStart)
{
    if (!m_mltProducer) {
        kDebug() << "PLAYLIST NOT INITIALISED //////";
        return 0;
    }
    Mlt::Producer parentProd(m_mltProducer->parent());
    if (parentProd.get_producer() == NULL) {
        kDebug() << "PLAYLIST BROKEN, CANNOT INSERT CLIP //////";
        return 0;
    }

    Mlt::Service service(parentProd.get_service());
    Mlt::Tractor tractor(service);
    int insertPos = pos.frames(m_fps);

    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
    int clipIndex = trackPlaylist.get_clip_index_at(insertPos);
    if (clipIndex == trackPlaylist.count()) {
        // We are after the end of the playlist
        return -1;
    }
    if (!trackPlaylist.is_blank(clipIndex)) return 0;
    if (fromBlankStart) return trackPlaylist.clip_length(clipIndex);
    return trackPlaylist.clip_length(clipIndex) + trackPlaylist.clip_start(clipIndex) - insertPos;
}

int Render::mltTrackDuration(int track)
{
    if (!m_mltProducer) {
        kDebug() << "PLAYLIST NOT INITIALISED //////";
        return -1;
    }
    Mlt::Producer parentProd(m_mltProducer->parent());
    if (parentProd.get_producer() == NULL) {
        kDebug() << "PLAYLIST BROKEN, CANNOT INSERT CLIP //////";
        return -1;
    }

    Mlt::Service service(parentProd.get_service());
    Mlt::Tractor tractor(service);

    Mlt::Producer trackProducer(tractor.track(track));
    return trackProducer.get_playtime() - 1;
}

void Render::mltInsertSpace(QMap <int, int> trackClipStartList, QMap <int, int> trackTransitionStartList, int track, const GenTime duration, const GenTime timeOffset)
{
    if (!m_mltProducer) {
        kDebug() << "PLAYLIST NOT INITIALISED //////";
        return;
    }
    Mlt::Producer parentProd(m_mltProducer->parent());
    if (parentProd.get_producer() == NULL) {
        kDebug() << "PLAYLIST BROKEN, CANNOT INSERT CLIP //////";
        return;
    }
    //kDebug()<<"// CLP STRT LST: "<<trackClipStartList;
    //kDebug()<<"// TRA STRT LST: "<<trackTransitionStartList;

    Mlt::Service service(parentProd.get_service());
    Mlt::Tractor tractor(service);
    mlt_service_lock(service.get_service());
    int diff = duration.frames(m_fps);
    int offset = timeOffset.frames(m_fps);
    int insertPos;

    if (track != -1) {
        // insert space in one track only
        Mlt::Producer trackProducer(tractor.track(track));
        Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
        insertPos = trackClipStartList.value(track);
        if (insertPos != -1) {
            insertPos += offset;
            int clipIndex = trackPlaylist.get_clip_index_at(insertPos);
            if (diff > 0) {
                trackPlaylist.insert_blank(clipIndex, diff - 1);
            } else {
                if (!trackPlaylist.is_blank(clipIndex)) clipIndex --;
                if (!trackPlaylist.is_blank(clipIndex)) {
                    kDebug() << "//// ERROR TRYING TO DELETE SPACE FROM " << insertPos;
                }
                int position = trackPlaylist.clip_start(clipIndex);
                int blankDuration = trackPlaylist.clip_length(clipIndex);
                if (blankDuration + diff == 0) {
                    trackPlaylist.remove(clipIndex);
                } else trackPlaylist.remove_region(position, -diff);
            }
            trackPlaylist.consolidate_blanks(0);
        }
        // now move transitions
        mlt_service serv = m_mltProducer->parent().get_service();
        mlt_service nextservice = mlt_service_get_producer(serv);
        mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
        QString mlt_type = mlt_properties_get(properties, "mlt_type");
        QString resource = mlt_properties_get(properties, "mlt_service");

        while (mlt_type == "transition") {
            mlt_transition tr = (mlt_transition) nextservice;
            int currentTrack = mlt_transition_get_b_track(tr);
            int currentIn = (int) mlt_transition_get_in(tr);
            int currentOut = (int) mlt_transition_get_out(tr);
            insertPos = trackTransitionStartList.value(track);
            if (insertPos != -1) {
                insertPos += offset;
                if (track == currentTrack && currentOut > insertPos && resource != "mix") {
                    mlt_transition_set_in_and_out(tr, currentIn + diff, currentOut + diff);
                }
            }
            nextservice = mlt_service_producer(nextservice);
            if (nextservice == NULL) break;
            properties = MLT_SERVICE_PROPERTIES(nextservice);
            mlt_type = mlt_properties_get(properties, "mlt_type");
            resource = mlt_properties_get(properties, "mlt_service");
        }
    } else {
        for (int trackNb = tractor.count() - 1; trackNb >= 1; --trackNb) {
            Mlt::Producer trackProducer(tractor.track(trackNb));
            Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());

            //int clipNb = trackPlaylist.count();
            insertPos = trackClipStartList.value(trackNb);
            if (insertPos != -1) {
                insertPos += offset;

                /* kDebug()<<"-------------\nTRACK "<<trackNb<<" HAS "<<clipNb<<" CLPIS";
                 kDebug() << "INSERT SPACE AT: "<<insertPos<<", DIFF: "<<diff<<", TK: "<<trackNb;
                        for (int i = 0; i < clipNb; i++) {
                            kDebug()<<"CLIP "<<i<<", START: "<<trackPlaylist.clip_start(i)<<", END: "<<trackPlaylist.clip_start(i) + trackPlaylist.clip_length(i);
                     if (trackPlaylist.is_blank(i)) kDebug()<<"++ BLANK ++ ";
                     kDebug()<<"-------------";
                 }
                 kDebug()<<"END-------------";*/


                int clipIndex = trackPlaylist.get_clip_index_at(insertPos);
                if (diff > 0) {
                    trackPlaylist.insert_blank(clipIndex, diff - 1);
                } else {
                    if (!trackPlaylist.is_blank(clipIndex)) {
                        clipIndex --;
                    }
                    if (!trackPlaylist.is_blank(clipIndex)) {
                        kDebug() << "//// ERROR TRYING TO DELETE SPACE FROM " << insertPos;
                    }
                    int position = trackPlaylist.clip_start(clipIndex);
                    int blankDuration = trackPlaylist.clip_length(clipIndex);
                    if (diff + blankDuration == 0) {
                        trackPlaylist.remove(clipIndex);
                    } else trackPlaylist.remove_region(position, - diff);
                }
                trackPlaylist.consolidate_blanks(0);
            }
        }
        // now move transitions
        mlt_service serv = m_mltProducer->parent().get_service();
        mlt_service nextservice = mlt_service_get_producer(serv);
        mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
        QString mlt_type = mlt_properties_get(properties, "mlt_type");
        QString resource = mlt_properties_get(properties, "mlt_service");

        while (mlt_type == "transition") {
            mlt_transition tr = (mlt_transition) nextservice;
            int currentIn = (int) mlt_transition_get_in(tr);
            int currentOut = (int) mlt_transition_get_out(tr);
            int currentTrack = mlt_transition_get_b_track(tr);
            insertPos = trackTransitionStartList.value(currentTrack);
            if (insertPos != -1) {
                insertPos += offset;
                if (currentOut > insertPos && resource != "mix") {
                    mlt_transition_set_in_and_out(tr, currentIn + diff, currentOut + diff);
                }
            }
            nextservice = mlt_service_producer(nextservice);
            if (nextservice == NULL) break;
            properties = MLT_SERVICE_PROPERTIES(nextservice);
            mlt_type = mlt_properties_get(properties, "mlt_type");
            resource = mlt_properties_get(properties, "mlt_service");
        }
    }
    mlt_service_unlock(service.get_service());
    mltCheckLength(&tractor);
    m_mltConsumer->set("refresh", 1);
}


void Render::mltPasteEffects(Mlt::Producer *source, Mlt::Producer *dest)
{
    if (source == dest) return;
    Mlt::Service sourceService(source->get_service());
    Mlt::Service destService(dest->get_service());

    // move all effects to the correct producer
    int ct = 0;
    Mlt::Filter *filter = sourceService.filter(ct);
    while (filter) {
        if (filter->get_int("kdenlive_ix") != 0) {
            sourceService.detach(*filter);
            destService.attach(*filter);
        } else ct++;
        filter = sourceService.filter(ct);
    }
}

int Render::mltChangeClipSpeed(ItemInfo info, ItemInfo speedIndependantInfo, double speed, double /*oldspeed*/, int strobe, Mlt::Producer *prod)
{
    m_isBlocked = true;
    int newLength = 0;
    Mlt::Service service(m_mltProducer->parent().get_service());
    if (service.type() != tractor_type) {
        kWarning() << "// TRACTOR PROBLEM";
        return -1;
    }
    
    //kDebug() << "Changing clip speed, set in and out: " << info.cropStart.frames(m_fps) << " to " << (info.endPos - info.startPos).frames(m_fps) - 1;
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(info.track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
    int startPos = info.startPos.frames(m_fps);
    int clipIndex = trackPlaylist.get_clip_index_at(startPos);
    int clipLength = trackPlaylist.clip_length(clipIndex);

    Mlt::Producer *original = trackPlaylist.get_clip(clipIndex);
    if (original == NULL) {
        return -1;
    }
    if (!original->is_valid() || original->is_blank()) {
        // invalid clip
        delete original;
        return -1;
    }
    Mlt::Producer clipparent = original->parent();
    if (!clipparent.is_valid() || clipparent.is_blank()) {
        // invalid clip
        delete original;
        return -1;
    }

    QString serv = clipparent.get("mlt_service");
    QString id = clipparent.get("id");
    if (speed <= 0 && speed > -1) speed = 1.0;
    //kDebug() << "CLIP SERVICE: " << serv;
    if ((serv == "avformat" || serv == "avformat-novalidate") && (speed != 1.0 || strobe > 1)) {
        mlt_service_lock(service.get_service());
        QString url = QString::fromUtf8(clipparent.get("resource"));
        url.append('?' + m_locale.toString(speed));
        if (strobe > 1) url.append("&strobe=" + QString::number(strobe));
        Mlt::Producer *slowprod = m_slowmotionProducers.value(url);
        if (!slowprod || slowprod->get_producer() == NULL) {
            slowprod = new Mlt::Producer(*m_mltProfile, 0, ("framebuffer:" + url).toUtf8().constData());
            if (strobe > 1) slowprod->set("strobe", strobe);
            QString producerid = "slowmotion:" + id + ':' + m_locale.toString(speed);
            if (strobe > 1) producerid.append(':' + QString::number(strobe));
            slowprod->set("id", producerid.toUtf8().constData());
            // copy producer props
            double ar = original->parent().get_double("force_aspect_ratio");
            if (ar != 0.0) slowprod->set("force_aspect_ratio", ar);
            double fps = original->parent().get_double("force_fps");
            if (fps != 0.0) slowprod->set("force_fps", fps);
            int threads = original->parent().get_int("threads");
            if (threads != 0) slowprod->set("threads", threads);
            if (original->parent().get("force_progressive"))
                slowprod->set("force_progressive", original->parent().get_int("force_progressive"));
            if (original->parent().get("force_tff"))
                slowprod->set("force_tff", original->parent().get_int("force_tff"));
            int ix = original->parent().get_int("video_index");
            if (ix != 0) slowprod->set("video_index", ix);
            int colorspace = original->parent().get_int("force_colorspace");
            if (colorspace != 0) slowprod->set("force_colorspace", colorspace);
            int full_luma = original->parent().get_int("set.force_full_luma");
            if (full_luma != 0) slowprod->set("set.force_full_luma", full_luma);
            m_slowmotionProducers.insert(url, slowprod);
        }
        Mlt::Producer *clip = trackPlaylist.replace_with_blank(clipIndex);
        trackPlaylist.consolidate_blanks(0);

        // Check that the blank space is long enough for our new duration
        clipIndex = trackPlaylist.get_clip_index_at(startPos);
        int blankEnd = trackPlaylist.clip_start(clipIndex) + trackPlaylist.clip_length(clipIndex);
        Mlt::Producer *cut;
        if (clipIndex + 1 < trackPlaylist.count() && (startPos + clipLength / speed > blankEnd)) {
            GenTime maxLength = GenTime(blankEnd, m_fps) - info.startPos;
            cut = slowprod->cut((int)(info.cropStart.frames(m_fps) / speed), (int)(info.cropStart.frames(m_fps) / speed + maxLength.frames(m_fps) - 1));
        } else cut = slowprod->cut((int)(info.cropStart.frames(m_fps) / speed), (int)((info.cropStart.frames(m_fps) + clipLength) / speed - 1));

        // move all effects to the correct producer
        mltPasteEffects(clip, cut);
        trackPlaylist.insert_at(startPos, cut, 1);
        delete cut;
        delete clip;
        clipIndex = trackPlaylist.get_clip_index_at(startPos);
        newLength = trackPlaylist.clip_length(clipIndex);
        mlt_service_unlock(service.get_service());
    } else if (speed == 1.0 && strobe < 2) {
        mlt_service_lock(service.get_service());

        Mlt::Producer *clip = trackPlaylist.replace_with_blank(clipIndex);
        trackPlaylist.consolidate_blanks(0);

        // Check that the blank space is long enough for our new duration
        clipIndex = trackPlaylist.get_clip_index_at(startPos);
        int blankEnd = trackPlaylist.clip_start(clipIndex) + trackPlaylist.clip_length(clipIndex);

        Mlt::Producer *cut;
        int originalStart = (int)(speedIndependantInfo.cropStart.frames(m_fps));
        if (clipIndex + 1 < trackPlaylist.count() && (info.startPos + speedIndependantInfo.cropDuration).frames(m_fps) > blankEnd) {
            GenTime maxLength = GenTime(blankEnd, m_fps) - info.startPos;
            cut = prod->cut(originalStart, (int)(originalStart + maxLength.frames(m_fps) - 1));
        } else cut = prod->cut(originalStart, (int)(originalStart + speedIndependantInfo.cropDuration.frames(m_fps)) - 1);

        // move all effects to the correct producer
        mltPasteEffects(clip, cut);

        trackPlaylist.insert_at(startPos, cut, 1);
        delete cut;
        delete clip;
        clipIndex = trackPlaylist.get_clip_index_at(startPos);
        newLength = trackPlaylist.clip_length(clipIndex);
        mlt_service_unlock(service.get_service());

    } else if (serv == "framebuffer") {
        mlt_service_lock(service.get_service());
        QString url = QString::fromUtf8(clipparent.get("resource"));
        url = url.section('?', 0, 0);
        url.append('?' + m_locale.toString(speed));
        if (strobe > 1) url.append("&strobe=" + QString::number(strobe));
        Mlt::Producer *slowprod = m_slowmotionProducers.value(url);
        if (!slowprod || slowprod->get_producer() == NULL) {
            slowprod = new Mlt::Producer(*m_mltProfile, 0, ("framebuffer:" + url).toUtf8().constData());
            slowprod->set("strobe", strobe);
            QString producerid = "slowmotion:" + id.section(':', 1, 1) + ':' + m_locale.toString(speed);
            if (strobe > 1) producerid.append(':' + QString::number(strobe));
            slowprod->set("id", producerid.toUtf8().constData());
            // copy producer props
            double ar = original->parent().get_double("force_aspect_ratio");
            if (ar != 0.0) slowprod->set("force_aspect_ratio", ar);
            double fps = original->parent().get_double("force_fps");
            if (fps != 0.0) slowprod->set("force_fps", fps);
            if (original->parent().get("force_progressive"))
                slowprod->set("force_progressive", original->parent().get_int("force_progressive"));
            if (original->parent().get("force_tff"))
                slowprod->set("force_tff", original->parent().get_int("force_tff"));
            int threads = original->parent().get_int("threads");
            if (threads != 0) slowprod->set("threads", threads);
            int ix = original->parent().get_int("video_index");
            if (ix != 0) slowprod->set("video_index", ix);
            int colorspace = original->parent().get_int("force_colorspace");
            if (colorspace != 0) slowprod->set("force_colorspace", colorspace);
            int full_luma = original->parent().get_int("set.force_full_luma");
            if (full_luma != 0) slowprod->set("set.force_full_luma", full_luma);
            m_slowmotionProducers.insert(url, slowprod);
        }
        Mlt::Producer *clip = trackPlaylist.replace_with_blank(clipIndex);
        trackPlaylist.consolidate_blanks(0);

        GenTime duration = speedIndependantInfo.cropDuration / speed;
        int originalStart = (int)(speedIndependantInfo.cropStart.frames(m_fps) / speed);

        // Check that the blank space is long enough for our new duration
        clipIndex = trackPlaylist.get_clip_index_at(startPos);
        int blankEnd = trackPlaylist.clip_start(clipIndex) + trackPlaylist.clip_length(clipIndex);

        Mlt::Producer *cut;
        if (clipIndex + 1 < trackPlaylist.count() && (info.startPos + duration).frames(m_fps) > blankEnd) {
            GenTime maxLength = GenTime(blankEnd, m_fps) - info.startPos;
            cut = slowprod->cut(originalStart, (int)(originalStart + maxLength.frames(m_fps) - 1));
        } else cut = slowprod->cut(originalStart, (int)(originalStart + duration.frames(m_fps)) - 1);

        // move all effects to the correct producer
        mltPasteEffects(clip, cut);

        trackPlaylist.insert_at(startPos, cut, 1);
        delete cut;
        delete clip;
        clipIndex = trackPlaylist.get_clip_index_at(startPos);
        newLength = trackPlaylist.clip_length(clipIndex);

        mlt_service_unlock(service.get_service());
    }
    delete original;
    if (clipIndex + 1 == trackPlaylist.count()) mltCheckLength(&tractor);
    m_isBlocked = false;
    return newLength;
}

bool Render::mltRemoveTrackEffect(int track, QString index, bool updateIndex)
{
    Mlt::Service service(m_mltProducer->parent().get_service());
    bool success = false;
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
    Mlt::Service clipService(trackPlaylist.get_service());

    m_isBlocked = true;
    mlt_service_lock(service.get_service());
    int ct = 0;
    Mlt::Filter *filter = clipService.filter(ct);
    while (filter) {
        if ((index == "-1" && strcmp(filter->get("kdenlive_id"), ""))  || filter->get_int("kdenlive_ix") == index.toInt()) {
            if (clipService.detach(*filter) == 0) success = true;
        } else if (updateIndex) {
            // Adjust the other effects index
            if (filter->get_int("kdenlive_ix") > index.toInt()) filter->set("kdenlive_ix", filter->get_int("kdenlive_ix") - 1);
            ct++;
        } else ct++;
        filter = clipService.filter(ct);
    }
    m_isBlocked = false;
    mlt_service_unlock(service.get_service());
    refresh();
    return success;
}

bool Render::mltRemoveEffect(int track, GenTime position, QString index, bool updateIndex, bool doRefresh)
{
    if (position < GenTime()) {
        // Remove track effect
        return mltRemoveTrackEffect(track, index, updateIndex);
    }
    Mlt::Service service(m_mltProducer->parent().get_service());
    bool success = false;
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());

    int clipIndex = trackPlaylist.get_clip_index_at((int) position.frames(m_fps));
    Mlt::Producer *clip = trackPlaylist.get_clip(clipIndex);
    if (!clip) {
        kDebug() << " / / / CANNOT FIND CLIP TO REMOVE EFFECT";
        return false;
    }

    Mlt::Service clipService(clip->get_service());
    int duration = clip->get_playtime();
    if (doRefresh) {
        // Check if clip is visible in monitor
        int diff = trackPlaylist.clip_start(clipIndex) + duration - m_mltProducer->position();
        if (diff < 0 || diff > duration) doRefresh = false;
    }
    delete clip;

//    if (tag.startsWith("ladspa")) tag = "ladspa";
    m_isBlocked = true;
    mlt_service_lock(service.get_service());
    int ct = 0;
    Mlt::Filter *filter = clipService.filter(ct);
    while (filter) {
        if ((index == "-1" && strcmp(filter->get("kdenlive_id"), ""))  || filter->get_int("kdenlive_ix") == index.toInt()) {// && filter->get("kdenlive_id") == id) {
            if (clipService.detach(*filter) == 0) success = true;
            //kDebug()<<"Deleted filter id:"<<filter->get("kdenlive_id")<<", ix:"<<filter->get("kdenlive_ix")<<", SERVICE:"<<filter->get("mlt_service");
        } else if (updateIndex) {
            // Adjust the other effects index
            if (filter->get_int("kdenlive_ix") > index.toInt()) filter->set("kdenlive_ix", filter->get_int("kdenlive_ix") - 1);
            ct++;
        } else ct++;
        filter = clipService.filter(ct);
    }
    m_isBlocked = false;
    mlt_service_unlock(service.get_service());
    if (doRefresh) refresh();
    return success;
}

bool Render::mltAddTrackEffect(int track, EffectsParameterList params)
{
    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
    Mlt::Service trackService(trackProducer.get_service()); //trackPlaylist
    return mltAddEffect(trackService, params, trackProducer.get_playtime() - 1, true);
}


bool Render::mltAddEffect(int track, GenTime position, EffectsParameterList params, bool doRefresh)
{

    Mlt::Service service(m_mltProducer->parent().get_service());

    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());

    int clipIndex = trackPlaylist.get_clip_index_at((int) position.frames(m_fps));
    Mlt::Producer *clip = trackPlaylist.get_clip(clipIndex);
    if (!clip) {
        return false;
    }

    Mlt::Service clipService(clip->get_service());
    int duration = clip->get_playtime();
    if (doRefresh) {
        // Check if clip is visible in monitor
        int diff = trackPlaylist.clip_start(clipIndex) + duration - m_mltProducer->position();
        if (diff < 0 || diff > duration) doRefresh = false;
    }
    delete clip;
    return mltAddEffect(clipService, params, duration, doRefresh);
}

bool Render::mltAddEffect(Mlt::Service service, EffectsParameterList params, int duration, bool doRefresh)
{
    bool updateIndex = false;
    const int filter_ix = params.paramValue("kdenlive_ix").toInt();
    const QString region =  params.paramValue("region");
    int ct = 0;
    m_isBlocked = true;
    mlt_service_lock(service.get_service());

    Mlt::Filter *filter = service.filter(ct);
    while (filter) {
        if (filter->get_int("kdenlive_ix") == filter_ix) {
            // A filter at that position already existed, so we will increase all indexes later
            updateIndex = true;
            break;
        }
        ct++;
        filter = service.filter(ct);
    }

    if (params.paramValue("id") == "speed") {
        // special case, speed effect is not really inserted, we just update the other effects index (kdenlive_ix)
        ct = 0;
        filter = service.filter(ct);
        while (filter) {
            if (filter->get_int("kdenlive_ix") >= filter_ix) {
                if (updateIndex) filter->set("kdenlive_ix", filter->get_int("kdenlive_ix") + 1);
            }
            ct++;
            filter = service.filter(ct);
        }
        m_isBlocked = false;
        mlt_service_unlock(service.get_service());
        if (doRefresh) refresh();
        return true;
    }


    // temporarily remove all effects after insert point
    QList <Mlt::Filter *> filtersList;
    ct = 0;
    filter = service.filter(ct);
    while (filter) {
        if (filter->get_int("kdenlive_ix") >= filter_ix) {
            filtersList.append(filter);
            service.detach(*filter);
        } else ct++;
        filter = service.filter(ct);
    }

    // create filter
    QString tag =  params.paramValue("tag");
    //kDebug() << " / / INSERTING EFFECT: " << tag << ", REGI: " << region;
    char *filterTag = qstrdup(tag.toUtf8().constData());
    char *filterId = qstrdup(params.paramValue("id").toUtf8().constData());
    QHash<QString, QString>::Iterator it;
    QString kfr = params.paramValue("keyframes");

    if (!kfr.isEmpty()) {
        QStringList keyFrames = kfr.split(';', QString::SkipEmptyParts);
        //kDebug() << "// ADDING KEYFRAME EFFECT: " << params.paramValue("keyframes");
        char *starttag = qstrdup(params.paramValue("starttag", "start").toUtf8().constData());
        char *endtag = qstrdup(params.paramValue("endtag", "end").toUtf8().constData());
        //kDebug() << "// ADDING KEYFRAME TAGS: " << starttag << ", " << endtag;
        //double max = params.paramValue("max").toDouble();
        double min = params.paramValue("min").toDouble();
        double factor = params.paramValue("factor", "1").toDouble();
        params.removeParam("starttag");
        params.removeParam("endtag");
        params.removeParam("keyframes");
        params.removeParam("min");
        params.removeParam("max");
        params.removeParam("factor");
        int offset = 0;
        // Special case, only one keyframe, means we want a constant value
        if (keyFrames.count() == 1) {
            Mlt::Filter *filter = new Mlt::Filter(*m_mltProfile, filterTag);
            if (filter && filter->is_valid()) {
                filter->set("kdenlive_id", filterId);
                int x1 = keyFrames.at(0).section(':', 0, 0).toInt();
                double y1 = keyFrames.at(0).section(':', 1, 1).toDouble();
                for (int j = 0; j < params.count(); j++) {
                    filter->set(params.at(j).name().toUtf8().constData(), params.at(j).value().toUtf8().constData());
                }
                filter->set("in", x1);
                //kDebug() << "// ADDING KEYFRAME vals: " << min<<" / "<<max<<", "<<y1<<", factor: "<<factor;
                filter->set(starttag, m_locale.toString((min + y1) / factor).toUtf8().data());
                service.attach(*filter);
            }
        } else for (int i = 0; i < keyFrames.size() - 1; ++i) {
                Mlt::Filter *filter = new Mlt::Filter(*m_mltProfile, filterTag);
                if (filter && filter->is_valid()) {
                    filter->set("kdenlive_id", filterId);
                    int x1 = keyFrames.at(i).section(':', 0, 0).toInt() + offset;
                    double y1 = keyFrames.at(i).section(':', 1, 1).toDouble();
                    int x2 = keyFrames.at(i + 1).section(':', 0, 0).toInt();
                    double y2 = keyFrames.at(i + 1).section(':', 1, 1).toDouble();
                    if (x2 == -1) x2 = duration;

                    for (int j = 0; j < params.count(); j++) {
                        filter->set(params.at(j).name().toUtf8().constData(), params.at(j).value().toUtf8().constData());
                    }

                    filter->set("in", x1);
                    filter->set("out", x2);
                    //kDebug() << "// ADDING KEYFRAME vals: " << min<<" / "<<max<<", "<<y1<<", factor: "<<factor;
                    filter->set(starttag, m_locale.toString((min + y1) / factor).toUtf8().data());
                    filter->set(endtag, m_locale.toString((min + y2) / factor).toUtf8().data());
                    service.attach(*filter);
                    offset = 1;
                }
            }
        delete[] starttag;
        delete[] endtag;
    } else {
        Mlt::Filter *filter;
        QString prefix;
        if (!region.isEmpty()) {
            filter = new Mlt::Filter(*m_mltProfile, "region");
        } else filter = new Mlt::Filter(*m_mltProfile, filterTag);
        if (filter && filter->is_valid()) {
            filter->set("kdenlive_id", filterId);
            if (!region.isEmpty()) {
                filter->set("resource", region.toUtf8().constData());
                filter->set("kdenlive_ix", params.paramValue("kdenlive_ix").toUtf8().constData());
                filter->set("filter0", filterTag);
                prefix = "filter0.";
                params.removeParam("id");
                params.removeParam("region");
                params.removeParam("kdenlive_ix");
            }
        } else {
            kDebug() << "filter is NULL";
            m_isBlocked = false;
            mlt_service_unlock(service.get_service());
            return false;
        }
        params.removeParam("kdenlive_id");
        if (params.hasParam("_sync_in_out")) {
            // This effect must sync in / out with parent clip
            params.removeParam("_sync_in_out");
            filter->set_in_and_out(service.get_int("in"), service.get_int("out"));
        }

        for (int j = 0; j < params.count(); j++) {
            filter->set((prefix + params.at(j).name()).toUtf8().constData(), params.at(j).value().toUtf8().constData());
        }

        if (tag == "sox") {
            QString effectArgs = params.paramValue("id").section('_', 1);

            params.removeParam("id");
            params.removeParam("kdenlive_ix");
            params.removeParam("tag");
            params.removeParam("disable");
            params.removeParam("region");

            for (int j = 0; j < params.count(); j++) {
                effectArgs.append(' ' + params.at(j).value());
            }
            //kDebug() << "SOX EFFECTS: " << effectArgs.simplified();
            filter->set("effect", effectArgs.simplified().toUtf8().constData());
        }

        // attach filter to the clip
        service.attach(*filter);
    }
    delete[] filterId;
    delete[] filterTag;

    // re-add following filters
    for (int i = 0; i < filtersList.count(); i++) {
        Mlt::Filter *filter = filtersList.at(i);
        if (updateIndex)
            filter->set("kdenlive_ix", filter->get_int("kdenlive_ix") + 1);
        service.attach(*filter);
    }
    m_isBlocked = false;
    mlt_service_unlock(service.get_service());
    if (doRefresh) refresh();
    return true;
}

bool Render::mltEditTrackEffect(int track, EffectsParameterList params)
{
    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
    Mlt::Service clipService(trackPlaylist.get_service());
    m_isBlocked = true;
    int ct = 0;
    QString index = params.paramValue("kdenlive_ix");
    QString tag =  params.paramValue("tag");

    Mlt::Filter *filter = clipService.filter(ct);
    while (filter) {
        if (filter->get_int("kdenlive_ix") == index.toInt()) {
            break;
        }
        ct++;
        filter = clipService.filter(ct);
    }

    if (!filter) {
        kDebug() << "WARINIG, FILTER FOR EDITING NOT FOUND, ADDING IT! " << index << ", " << tag;
        // filter was not found, it was probably a disabled filter, so add it to the correct place...

        bool success = false;//mltAddTrackEffect(track, params);
        m_isBlocked = false;
        return success;
    }
    QString prefix;
    QString ser = filter->get("mlt_service");
    if (ser == "region") prefix = "filter0.";
    mlt_service_lock(service.get_service());
    for (int j = 0; j < params.count(); j++) {
        filter->set((prefix + params.at(j).name()).toUtf8().constData(), params.at(j).value().toUtf8().constData());
    }
    mlt_service_unlock(service.get_service());

    m_isBlocked = false;
    refresh();
    return true;

}

bool Render::mltEditEffect(int track, GenTime position, EffectsParameterList params)
{
    QString index = params.paramValue("kdenlive_ix");
    QString tag =  params.paramValue("tag");

    if (!params.paramValue("keyframes").isEmpty() || /*it.key().startsWith("#") || */tag.startsWith("ladspa") || tag == "sox" || tag == "autotrack_rectangle" || params.hasParam("region")) {
        // This is a keyframe effect, to edit it, we remove it and re-add it.
        bool success = mltRemoveEffect(track, position, index, false);
//         if (!success) kDebug() << "// ERROR Removing effect : " << index;
        if (position < GenTime())
            success = mltAddTrackEffect(track, params);
        else
            success = mltAddEffect(track, position, params);
//         if (!success) kDebug() << "// ERROR Adding effect : " << index;
        return success;
    }
    if (position < GenTime()) {
        return mltEditTrackEffect(track, params);
    }
    // find filter
    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());

    int clipIndex = trackPlaylist.get_clip_index_at((int) position.frames(m_fps));
    Mlt::Producer *clip = trackPlaylist.get_clip(clipIndex);
    if (!clip) {
        kDebug() << "WARINIG, CANNOT FIND CLIP ON track: " << track << ", AT POS: " << position.frames(m_fps);
        return false;
    }

    int duration = clip->get_playtime();
    bool doRefresh = true;
    // Check if clip is visible in monitor
    int diff = trackPlaylist.clip_start(clipIndex) + duration - m_mltProducer->position();
    if (diff < 0 || diff > duration)
        doRefresh = false;
    m_isBlocked = true;
    int ct = 0;

    Mlt::Filter *filter = clip->filter(ct);
    while (filter) {
        if (filter->get_int("kdenlive_ix") == index.toInt()) {
            break;
        }
        ct++;
        filter = clip->filter(ct);
    }

    if (!filter) {
        kDebug() << "WARINIG, FILTER FOR EDITING NOT FOUND, ADDING IT! " << index << ", " << tag;
        // filter was not found, it was probably a disabled filter, so add it to the correct place...

        bool success = mltAddEffect(track, position, params);
        m_isBlocked = false;
        return success;
    }
    QString prefix;
    QString ser = filter->get("mlt_service");
    if (ser == "region") prefix = "filter0.";
    if (params.hasParam("_sync_in_out")) {
        // This effect must sync in / out with parent clip
        params.removeParam("_sync_in_out");
        filter->set_in_and_out(clip->get_in(), clip->get_out());
    }
    mlt_service_lock(service.get_service());
    for (int j = 0; j < params.count(); j++) {
        filter->set((prefix + params.at(j).name()).toUtf8().constData(), params.at(j).value().toUtf8().constData());
    }   

    delete clip;
    mlt_service_unlock(service.get_service());

    m_isBlocked = false;
    if (doRefresh) refresh();
    return true;
}

void Render::mltUpdateEffectPosition(int track, GenTime position, int oldPos, int newPos)
{
    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());

    int clipIndex = trackPlaylist.get_clip_index_at((int) position.frames(m_fps));
    Mlt::Producer *clip = trackPlaylist.get_clip(clipIndex);
    if (!clip) {
        kDebug() << "WARINIG, CANNOT FIND CLIP ON track: " << track << ", AT POS: " << position.frames(m_fps);
        return;
    }

    Mlt::Service clipService(clip->get_service());
    int duration = clip->get_playtime();
    bool doRefresh = true;
    // Check if clip is visible in monitor
    int diff = trackPlaylist.clip_start(clipIndex) + duration - m_mltProducer->position();
    if (diff < 0 || diff > duration) doRefresh = false;
    delete clip;

    m_isBlocked = true;
    int ct = 0;
    Mlt::Filter *filter = clipService.filter(ct);
    while (filter) {
        int pos = filter->get_int("kdenlive_ix");
        if (pos == oldPos) {
            filter->set("kdenlive_ix", newPos);
        } else ct++;
        filter = clipService.filter(ct);
    }

    m_isBlocked = false;
    if (doRefresh) refresh();
}

void Render::mltMoveEffect(int track, GenTime position, int oldPos, int newPos)
{
    if (position < GenTime()) {
        mltMoveTrackEffect(track, oldPos, newPos);
        return;
    }
    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());

    int clipIndex = trackPlaylist.get_clip_index_at((int) position.frames(m_fps));
    Mlt::Producer *clip = trackPlaylist.get_clip(clipIndex);
    if (!clip) {
        kDebug() << "WARINIG, CANNOT FIND CLIP ON track: " << track << ", AT POS: " << position.frames(m_fps);
        return;
    }

    Mlt::Service clipService(clip->get_service());
    int duration = clip->get_playtime();
    bool doRefresh = true;
    // Check if clip is visible in monitor
    int diff = trackPlaylist.clip_start(clipIndex) + duration - m_mltProducer->position();
    if (diff < 0 || diff > duration) doRefresh = false;
    delete clip;

    m_isBlocked = true;
    int ct = 0;
    QList <Mlt::Filter *> filtersList;
    Mlt::Filter *filter = clipService.filter(ct);
    bool found = false;
    if (newPos > oldPos) {
        while (filter) {
            if (!found && filter->get_int("kdenlive_ix") == oldPos) {
                filter->set("kdenlive_ix", newPos);
                filtersList.append(filter);
                clipService.detach(*filter);
                filter = clipService.filter(ct);
                while (filter && filter->get_int("kdenlive_ix") <= newPos) {
                    filter->set("kdenlive_ix", filter->get_int("kdenlive_ix") - 1);
                    ct++;
                    filter = clipService.filter(ct);
                }
                found = true;
            }
            if (filter && filter->get_int("kdenlive_ix") > newPos) {
                filtersList.append(filter);
                clipService.detach(*filter);
            } else ct++;
            filter = clipService.filter(ct);
        }
    } else {
        while (filter) {
            if (filter->get_int("kdenlive_ix") == oldPos) {
                filter->set("kdenlive_ix", newPos);
                filtersList.append(filter);
                clipService.detach(*filter);
            } else ct++;
            filter = clipService.filter(ct);
        }

        ct = 0;
        filter = clipService.filter(ct);
        while (filter) {
            int pos = filter->get_int("kdenlive_ix");
            if (pos >= newPos) {
                if (pos < oldPos) filter->set("kdenlive_ix", pos + 1);
                filtersList.append(filter);
                clipService.detach(*filter);
            } else ct++;
            filter = clipService.filter(ct);
        }
    }

    for (int i = 0; i < filtersList.count(); i++) {
        clipService.attach(*(filtersList.at(i)));
    }

    m_isBlocked = false;
    if (doRefresh) refresh();
}

void Render::mltMoveTrackEffect(int track, int oldPos, int newPos)
{
    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());

    Mlt::Service clipService(trackPlaylist.get_service());

    m_isBlocked = true;
    int ct = 0;
    QList <Mlt::Filter *> filtersList;
    Mlt::Filter *filter = clipService.filter(ct);
    bool found = false;
    if (newPos > oldPos) {
        while (filter) {
            if (!found && filter->get_int("kdenlive_ix") == oldPos) {
                filter->set("kdenlive_ix", newPos);
                filtersList.append(filter);
                clipService.detach(*filter);
                filter = clipService.filter(ct);
                while (filter && filter->get_int("kdenlive_ix") <= newPos) {
                    filter->set("kdenlive_ix", filter->get_int("kdenlive_ix") - 1);
                    ct++;
                    filter = clipService.filter(ct);
                }
                found = true;
            }
            if (filter && filter->get_int("kdenlive_ix") > newPos) {
                filtersList.append(filter);
                clipService.detach(*filter);
            } else ct++;
            filter = clipService.filter(ct);
        }
    } else {
        while (filter) {
            if (filter->get_int("kdenlive_ix") == oldPos) {
                filter->set("kdenlive_ix", newPos);
                filtersList.append(filter);
                clipService.detach(*filter);
            } else ct++;
            filter = clipService.filter(ct);
        }

        ct = 0;
        filter = clipService.filter(ct);
        while (filter) {
            int pos = filter->get_int("kdenlive_ix");
            if (pos >= newPos) {
                if (pos < oldPos) filter->set("kdenlive_ix", pos + 1);
                filtersList.append(filter);
                clipService.detach(*filter);
            } else ct++;
            filter = clipService.filter(ct);
        }
    }

    for (int i = 0; i < filtersList.count(); i++) {
        clipService.attach(*(filtersList.at(i)));
    }
    m_isBlocked = false;
    refresh();
}

bool Render::mltResizeClipEnd(ItemInfo info, GenTime clipDuration)
{
    m_isBlocked = true;
    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(info.track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());

    /* // Display playlist info
    kDebug()<<"////////////  BEFORE RESIZE";
    for (int i = 0; i < trackPlaylist.count(); i++) {
    int blankStart = trackPlaylist.clip_start(i);
    int blankDuration = trackPlaylist.clip_length(i) - 1;
    QString blk;
    if (trackPlaylist.is_blank(i)) blk = "(blank)";
    kDebug()<<"CLIP "<<i<<": ("<<blankStart<<'x'<<blankStart + blankDuration<<")"<<blk;
    }*/

    if (trackPlaylist.is_blank_at((int) info.startPos.frames(m_fps))) {
        kDebug() << "////////  ERROR RSIZING BLANK CLIP!!!!!!!!!!!";
        m_isBlocked = false;
        return false;
    }
    mlt_service_lock(service.get_service());
    int clipIndex = trackPlaylist.get_clip_index_at((int) info.startPos.frames(m_fps));
    //kDebug() << "// SELECTED CLIP START: " << trackPlaylist.clip_start(clipIndex);
    Mlt::Producer *clip = trackPlaylist.get_clip(clipIndex);

    int previousStart = clip->get_in();
    int newDuration = (int) clipDuration.frames(m_fps) - 1;
    int diff = newDuration - (trackPlaylist.clip_length(clipIndex) - 1);

    int currentOut = newDuration + previousStart;
    if (currentOut > clip->get_length()) {
        clip->parent().set("length", currentOut + 1);
        clip->parent().set("out", currentOut);
        clip->set("length", currentOut + 1);
    }

    /*if (newDuration > clip->get_out()) {
        clip->parent().set_in_and_out(0, newDuration + 1);
        clip->set_in_and_out(0, newDuration + 1);
    }*/
    delete clip;
    trackPlaylist.resize_clip(clipIndex, previousStart, newDuration + previousStart);
    trackPlaylist.consolidate_blanks(0);
    // skip to next clip
    clipIndex++;
    //kDebug() << "////////  RESIZE CLIP: " << clipIndex << "( pos: " << info.startPos.frames(25) << "), DIFF: " << diff << ", CURRENT DUR: " << previousDuration << ", NEW DUR: " << newDuration << ", IX: " << clipIndex << ", MAX: " << trackPlaylist.count();
    if (diff > 0) {
        // clip was made longer, trim next blank if there is one.
        if (clipIndex < trackPlaylist.count()) {
            // If this is not the last clip in playlist
            if (trackPlaylist.is_blank(clipIndex)) {
                int blankStart = trackPlaylist.clip_start(clipIndex);
                int blankDuration = trackPlaylist.clip_length(clipIndex);
                if (diff > blankDuration) {
                    kDebug() << "// ERROR blank clip is not large enough to get back required space!!!";
                }
                if (diff - blankDuration == 0) {
                    trackPlaylist.remove(clipIndex);
                } else trackPlaylist.remove_region(blankStart, diff);
            } else {
                kDebug() << "/// RESIZE ERROR, NXT CLIP IS NOT BLK: " << clipIndex;
            }
        }
    } else if (clipIndex != trackPlaylist.count()) trackPlaylist.insert_blank(clipIndex, 0 - diff - 1);
    trackPlaylist.consolidate_blanks(0);
    mlt_service_unlock(service.get_service());

    if (info.track != 0 && clipIndex == trackPlaylist.count()) mltCheckLength(&tractor);
    /*if (QString(clip->parent().get("transparency")).toInt() == 1) {
        //mltResizeTransparency(previousStart, previousStart, previousStart + newDuration, track, QString(clip->parent().get("id")).toInt());
        mltDeleteTransparency(info.startPos.frames(m_fps), info.track, QString(clip->parent().get("id")).toInt());
        ItemInfo transpinfo;
        transpinfo.startPos = info.startPos;
        transpinfo.endPos = info.startPos + clipDuration;
        transpinfo.track = info.track;
        mltAddClipTransparency(transpinfo, info.track - 1, QString(clip->parent().get("id")).toInt());
    }*/
    m_isBlocked = false;
    m_mltConsumer->set("refresh", 1);
    return true;
}

void Render::mltChangeTrackState(int track, bool mute, bool blind)
{
    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(track));

    // Make sure muting will not produce problems with our audio mixing transition,
    // because audio mixing is done between each track and the lowest one
    bool audioMixingBroken = false;
    if (mute && trackProducer.get_int("hide") < 2 ) {
            // We mute a track with sound
            if (track == getLowestNonMutedAudioTrack(tractor)) audioMixingBroken = true;
    }
    else if (!mute && trackProducer.get_int("hide") > 1 ) {
            // We un-mute a previously muted track
            if (track < getLowestNonMutedAudioTrack(tractor)) audioMixingBroken = true;
    }

    if (mute) {
        if (blind) trackProducer.set("hide", 3);
        else trackProducer.set("hide", 2);
    } else if (blind) {
        trackProducer.set("hide", 1);
    } else {
        trackProducer.set("hide", 0);
    }
    if (audioMixingBroken) fixAudioMixing(tractor);
    
    tractor.multitrack()->refresh();
    tractor.refresh();
    refresh();
}

int Render::getLowestNonMutedAudioTrack(Mlt::Tractor tractor)
{
    for (int i = 1; i < tractor.count(); i++) {
        Mlt::Producer trackProducer(tractor.track(i));
        if (trackProducer.get_int("hide") < 2) return i;
    }
    return tractor.count() - 1;
}

void Render::fixAudioMixing(Mlt::Tractor tractor)
{
    // Make sure the audio mixing transitions are applied to the lowest audible (non muted) track
    int lowestTrack = getLowestNonMutedAudioTrack(tractor);

    mlt_service serv = m_mltProducer->parent().get_service();
    Mlt::Field *field = tractor.field();
    mlt_service_lock(serv);
    m_isBlocked++;

    mlt_service nextservice = mlt_service_get_producer(serv);
    mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
    QString mlt_type = mlt_properties_get(properties, "mlt_type");
    QString resource = mlt_properties_get(properties, "mlt_service");

    mlt_service nextservicetodisconnect;
     // Delete all audio mixing transitions
    while (mlt_type == "transition") {
        if (resource == "mix") {
            nextservicetodisconnect = nextservice;
            nextservice = mlt_service_producer(nextservice);
            mlt_field_disconnect_service(field->get_field(), nextservicetodisconnect);
        }
        else nextservice = mlt_service_producer(nextservice);
        if (nextservice == NULL) break;
        properties = MLT_SERVICE_PROPERTIES(nextservice);
        mlt_type = mlt_properties_get(properties, "mlt_type");
        resource = mlt_properties_get(properties, "mlt_service");
    }

    // Re-add correct audio transitions
    for (int i = lowestTrack + 1; i < tractor.count(); i++) {
        Mlt::Transition *transition = new Mlt::Transition(*m_mltProfile, "mix");
        transition->set("always_active", 1);
        transition->set("combine", 1);
        transition->set("internal_added", 237);
        field->plant_transition(*transition, lowestTrack, i);
    }
    mlt_service_unlock(serv);
    m_isBlocked--;    
}

bool Render::mltResizeClipCrop(ItemInfo info, GenTime diff)
{
    Mlt::Service service(m_mltProducer->parent().get_service());
    int frameOffset = (int) diff.frames(m_fps);
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(info.track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
    if (trackPlaylist.is_blank_at(info.startPos.frames(m_fps))) {
        kDebug() << "////////  ERROR RSIZING BLANK CLIP!!!!!!!!!!!";
        return false;
    }
    mlt_service_lock(service.get_service());
    int clipIndex = trackPlaylist.get_clip_index_at(info.startPos.frames(m_fps));
    Mlt::Producer *clip = trackPlaylist.get_clip(clipIndex);
    if (clip == NULL) {
        kDebug() << "////////  ERROR RSIZING NULL CLIP!!!!!!!!!!!";
        mlt_service_unlock(service.get_service());
        return false;
    }
    int previousStart = clip->get_in();
    int previousOut = clip->get_out();
    delete clip;
    m_isBlocked = true;
    trackPlaylist.resize_clip(clipIndex, previousStart + frameOffset, previousOut + frameOffset);
    m_isBlocked = false;
    mlt_service_unlock(service.get_service());
    m_mltConsumer->set("refresh", 1);
    return true;
}

bool Render::mltResizeClipStart(ItemInfo info, GenTime diff)
{
    //kDebug() << "////////  RSIZING CLIP from: "<<info.startPos.frames(25)<<" to "<<diff.frames(25);
    Mlt::Service service(m_mltProducer->parent().get_service());
    int moveFrame = (int) diff.frames(m_fps);
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(info.track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
    if (trackPlaylist.is_blank_at(info.startPos.frames(m_fps))) {
        kDebug() << "////////  ERROR RSIZING BLANK CLIP!!!!!!!!!!!";
        return false;
    }
    mlt_service_lock(service.get_service());
    int clipIndex = trackPlaylist.get_clip_index_at(info.startPos.frames(m_fps));
    Mlt::Producer *clip = trackPlaylist.get_clip(clipIndex);
    if (clip == NULL || clip->is_blank()) {
        kDebug() << "////////  ERROR RSIZING NULL CLIP!!!!!!!!!!!";
        mlt_service_unlock(service.get_service());
        return false;
    }
    int previousStart = clip->get_in();
    int previousOut = clip->get_out();

    m_isBlocked = true;
    previousStart += moveFrame;

    if (previousStart < 0) {
        // this is possible for images and color clips
        previousOut -= previousStart;
        previousStart = 0;
    }

    int length = previousOut + 1;
    if (length > clip->get_length()) {
        clip->parent().set("length", length + 1);
        clip->parent().set("out", length);
        clip->set("length", length + 1);
    }
    delete clip;

    // kDebug() << "RESIZE, new start: " << previousStart << ", " << previousOut;
    trackPlaylist.resize_clip(clipIndex, previousStart, previousOut);
    if (moveFrame > 0) {
        trackPlaylist.insert_blank(clipIndex, moveFrame - 1);
    } else {
        //int midpos = info.startPos.frames(m_fps) + moveFrame - 1;
        int blankIndex = clipIndex - 1;
        int blankLength = trackPlaylist.clip_length(blankIndex);
        // kDebug() << " + resizing blank length " <<  blankLength << ", SIZE DIFF: " << moveFrame;
        if (! trackPlaylist.is_blank(blankIndex)) {
            kDebug() << "WARNING, CLIP TO RESIZE IS NOT BLANK";
        }
        if (blankLength + moveFrame == 0)
            trackPlaylist.remove(blankIndex);
        else
            trackPlaylist.resize_clip(blankIndex, 0, blankLength + moveFrame - 1);
    }
    trackPlaylist.consolidate_blanks(0);
    /*if (QString(clip->parent().get("transparency")).toInt() == 1) {
        //mltResizeTransparency(previousStart, (int) moveEnd.frames(m_fps), (int) (moveEnd + out - in).frames(m_fps), track, QString(clip->parent().get("id")).toInt());
        mltDeleteTransparency(info.startPos.frames(m_fps), info.track, QString(clip->parent().get("id")).toInt());
        ItemInfo transpinfo;
        transpinfo.startPos = info.startPos + diff;
        transpinfo.endPos = info.startPos + diff + (info.endPos - info.startPos);
        transpinfo.track = info.track;
        mltAddClipTransparency(transpinfo, info.track - 1, QString(clip->parent().get("id")).toInt());
    }*/
    m_isBlocked = false;
    //m_mltConsumer->set("refresh", 1);
    mlt_service_unlock(service.get_service());
    m_mltConsumer->set("refresh", 1);
    return true;
}

bool Render::mltMoveClip(int startTrack, int endTrack, GenTime moveStart, GenTime moveEnd, Mlt::Producer *prod, bool overwrite, bool insert)
{
    return mltMoveClip(startTrack, endTrack, (int) moveStart.frames(m_fps), (int) moveEnd.frames(m_fps), prod, overwrite, insert);
}


bool Render::mltUpdateClipProducer(int track, int pos, Mlt::Producer *prod)
{
    if (prod == NULL || !prod->is_valid()) {
        kDebug() << "// Warning, CLIP on track " << track << ", at: " << pos << " is invalid, cannot update it!!!";
        return false;
    }
    m_isBlocked++;
    //kDebug() << "// TRYING TO UPDATE CLIP at: " << pos << ", TK: " << track;
    Mlt::Service service(m_mltProducer->parent().get_service());
    if (service.type() != tractor_type) {
        kWarning() << "// TRACTOR PROBLEM";
        return false;
    }
    mlt_service_lock(service.get_service());
    Mlt::Tractor tractor(service);
    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
    int clipIndex = trackPlaylist.get_clip_index_at(pos);
    Mlt::Producer *clipProducer = trackPlaylist.replace_with_blank(clipIndex);
    if (clipProducer == NULL || clipProducer->is_blank()) {
        kDebug() << "// ERROR UPDATING CLIP PROD";
        delete clipProducer;
        mlt_service_unlock(service.get_service());
        m_isBlocked--;
        return false;
    }
    Mlt::Producer *clip = prod->cut(clipProducer->get_in(), clipProducer->get_out());
    if (!clip || !clip->is_valid()) {
        if (clip) delete clip;
        delete clipProducer;
        mlt_service_unlock(service.get_service());
        m_isBlocked--;
        return false;
    }
    // move all effects to the correct producer
    mltPasteEffects(clipProducer, clip);
    trackPlaylist.insert_at(pos, clip, 1);
    delete clip;
    delete clipProducer;
    mlt_service_unlock(service.get_service());
    m_isBlocked--;
    return true;
}

bool Render::mltMoveClip(int startTrack, int endTrack, int moveStart, int moveEnd, Mlt::Producer *prod, bool overwrite, bool /*insert*/)
{
    m_isBlocked++;

    Mlt::Service service(m_mltProducer->parent().get_service());
    if (service.type() != tractor_type) {
        kWarning() << "// TRACTOR PROBLEM";
        return false;
    }

    Mlt::Tractor tractor(service);
    mlt_service_lock(service.get_service());
    Mlt::Producer trackProducer(tractor.track(startTrack));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
    int clipIndex = trackPlaylist.get_clip_index_at(moveStart);
    //kDebug() << "//////  LOOKING FOR CLIP TO MOVE, INDEX: " << clipIndex;
    bool checkLength = false;
    if (endTrack == startTrack) {
        Mlt::Producer *clipProducer = trackPlaylist.replace_with_blank(clipIndex);
        if ((!overwrite && !trackPlaylist.is_blank_at(moveEnd)) || !clipProducer || !clipProducer->is_valid() || clipProducer->is_blank()) {
            // error, destination is not empty
            if (clipProducer) {
                if (!trackPlaylist.is_blank_at(moveEnd) && clipProducer->is_valid()) trackPlaylist.insert_at(moveStart, clipProducer, 1);
                delete clipProducer;
            }
            //int ix = trackPlaylist.get_clip_index_at(moveEnd);
            kDebug() << "// ERROR MOVING CLIP TO : " << moveEnd;
            mlt_service_unlock(service.get_service());
            m_isBlocked--;
            return false;
        } else {
            trackPlaylist.consolidate_blanks(0);
            if (overwrite) {
                trackPlaylist.remove_region(moveEnd, clipProducer->get_playtime());
                int clipIndex = trackPlaylist.get_clip_index_at(moveEnd);
                trackPlaylist.insert_blank(clipIndex, clipProducer->get_playtime() - 1);
            }
            int newIndex = trackPlaylist.insert_at(moveEnd, clipProducer, 1);
            trackPlaylist.consolidate_blanks(1);
            delete clipProducer;
            /*if (QString(clipProducer.parent().get("transparency")).toInt() == 1) {
            mltMoveTransparency(moveStart, moveEnd, startTrack, endTrack, QString(clipProducer.parent().get("id")).toInt());
            }*/
            if (newIndex + 1 == trackPlaylist.count()) checkLength = true;
        }
        //mlt_service_unlock(service.get_service());
    } else {
        Mlt::Producer destTrackProducer(tractor.track(endTrack));
        Mlt::Playlist destTrackPlaylist((mlt_playlist) destTrackProducer.get_service());
        if (!overwrite && !destTrackPlaylist.is_blank_at(moveEnd)) {
            // error, destination is not empty
            mlt_service_unlock(service.get_service());
            m_isBlocked--;
            return false;
        } else {
            Mlt::Producer *clipProducer = trackPlaylist.replace_with_blank(clipIndex);
            if (!clipProducer || clipProducer->is_blank()) {
                // error, destination is not empty
                //int ix = trackPlaylist.get_clip_index_at(moveEnd);
                if (clipProducer) delete clipProducer;
                kDebug() << "// ERROR MOVING CLIP TO : " << moveEnd;
                mlt_service_unlock(service.get_service());
                m_isBlocked--;
                return false;
            }
            trackPlaylist.consolidate_blanks(0);
            destTrackPlaylist.consolidate_blanks(1);
            Mlt::Producer *clip;
            // check if we are moving a slowmotion producer
            QString serv = clipProducer->parent().get("mlt_service");
            QString currentid = clipProducer->parent().get("id");
            if (serv == "framebuffer" || currentid.endsWith("_video")) {
                clip = clipProducer;
            } else {
                if (prod == NULL) {
                    // Special case: prod is null when using placeholder clips.
                    // in that case, use the producer existing in playlist. Note that
                    // it will bypass the one producer per track logic and might cause
                    // Sound cracks if clip is moved so that it overlaps another copy of itself
                    clip = clipProducer->cut(clipProducer->get_in(), clipProducer->get_out());
                } else clip = prod->cut(clipProducer->get_in(), clipProducer->get_out());
            }

            // move all effects to the correct producer
            mltPasteEffects(clipProducer, clip);

            if (overwrite) {
                destTrackPlaylist.remove_region(moveEnd, clip->get_playtime());
                int clipIndex = destTrackPlaylist.get_clip_index_at(moveEnd);
                destTrackPlaylist.insert_blank(clipIndex, clip->get_playtime() - 1);
            }

            int newIndex = destTrackPlaylist.insert_at(moveEnd, clip, 1);

            if (clip == clipProducer) {
                delete clip;
                clip = NULL;
            } else {
                delete clip;
                delete clipProducer;
            }
            destTrackPlaylist.consolidate_blanks(0);
            /*if (QString(clipProducer.parent().get("transparency")).toInt() == 1) {
                kDebug() << "//////// moving clip transparency";
                mltMoveTransparency(moveStart, moveEnd, startTrack, endTrack, QString(clipProducer.parent().get("id")).toInt());
            }*/
            if (clipIndex > trackPlaylist.count()) checkLength = true;
            else if (newIndex + 1 == destTrackPlaylist.count()) checkLength = true;
        }
    }
    mlt_service_unlock(service.get_service());
    if (checkLength) mltCheckLength(&tractor);
    m_isBlocked--;
    //askForRefresh();
    //m_mltConsumer->set("refresh", 1);
    return true;
}


QList <int> Render::checkTrackSequence(int track)
{
    QList <int> list;
    Mlt::Service service(m_mltProducer->parent().get_service());
    if (service.type() != tractor_type) {
        kWarning() << "// TRACTOR PROBLEM";
        return list;
    }
    Mlt::Tractor tractor(service);
    mlt_service_lock(service.get_service());
    Mlt::Producer trackProducer(tractor.track(track));
    Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
    int clipNb = trackPlaylist.count();
    //kDebug() << "// PARSING SCENE TRACK: " << t << ", CLIPS: " << clipNb;
    for (int i = 0; i < clipNb; i++) {
        Mlt::Producer *c = trackPlaylist.get_clip(i);
        int pos = trackPlaylist.clip_start(i);
        if (!list.contains(pos)) list.append(pos);
        pos += c->get_playtime();
        if (!list.contains(pos)) list.append(pos);
        delete c;
    }
    return list;
}

bool Render::mltMoveTransition(QString type, int startTrack, int newTrack, int newTransitionTrack, GenTime oldIn, GenTime oldOut, GenTime newIn, GenTime newOut)
{
    int new_in = (int)newIn.frames(m_fps);
    int new_out = (int)newOut.frames(m_fps) - 1;
    if (new_in >= new_out) return false;
    int old_in = (int)oldIn.frames(m_fps);
    int old_out = (int)oldOut.frames(m_fps) - 1;

    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);
    Mlt::Field *field = tractor.field();

    bool doRefresh = true;
    // Check if clip is visible in monitor
    int diff = old_out - m_mltProducer->position();
    if (diff < 0 || diff > old_out - old_in) doRefresh = false;
    if (doRefresh) {
        diff = new_out - m_mltProducer->position();
        if (diff < 0 || diff > new_out - new_in) doRefresh = false;
    }

    m_isBlocked++;
    mlt_service_lock(service.get_service());

    mlt_service nextservice = mlt_service_get_producer(service.get_service());
    mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
    QString mlt_type = mlt_properties_get(properties, "mlt_type");
    QString resource = mlt_properties_get(properties, "mlt_service");
    int old_pos = (int)(old_in + old_out) / 2;
    bool found = false;

    while (mlt_type == "transition") {
        Mlt::Transition transition((mlt_transition) nextservice);
        int currentTrack = transition.get_b_track();
        int currentIn = (int) transition.get_in();
        int currentOut = (int) transition.get_out();

        if (resource == type && startTrack == currentTrack && currentIn <= old_pos && currentOut >= old_pos) {
            found = true;
            if (newTrack - startTrack != 0) {
                Mlt::Properties trans_props(transition.get_properties());
                Mlt::Transition new_transition(*m_mltProfile, transition.get("mlt_service"));
                Mlt::Properties new_trans_props(new_transition.get_properties());
                new_trans_props.inherit(trans_props);
                new_transition.set_in_and_out(new_in, new_out);
                field->disconnect_service(transition);
                mltPlantTransition(field, new_transition, newTransitionTrack, newTrack);
                //field->plant_transition(new_transition, newTransitionTrack, newTrack);
            } else transition.set_in_and_out(new_in, new_out);
            break;
        }
        nextservice = mlt_service_producer(nextservice);
        if (nextservice == NULL) break;
        properties = MLT_SERVICE_PROPERTIES(nextservice);
        mlt_type = mlt_properties_get(properties, "mlt_type");
        resource = mlt_properties_get(properties, "mlt_service");
    }
    mlt_service_unlock(service.get_service());
    m_isBlocked--;
    if (doRefresh) refresh();
    //if (m_isBlocked == 0) m_mltConsumer->set("refresh", 1);
    return found;
}


void Render::mltPlantTransition(Mlt::Field *field, Mlt::Transition &tr, int a_track, int b_track)
{
    mlt_service nextservice = mlt_service_get_producer(field->get_service());
    mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
    QString mlt_type = mlt_properties_get(properties, "mlt_type");
    QString resource = mlt_properties_get(properties, "mlt_service");
    QList <Mlt::Transition *> trList;

    while (mlt_type == "transition") {
        Mlt::Transition transition((mlt_transition) nextservice);
        int aTrack = transition.get_a_track();
        int bTrack = transition.get_b_track();
        if (resource != "mix" && (aTrack < a_track || (aTrack == a_track && bTrack > b_track))) {
            Mlt::Properties trans_props(transition.get_properties());
            Mlt::Transition *cp = new Mlt::Transition(*m_mltProfile, transition.get("mlt_service"));
            Mlt::Properties new_trans_props(cp->get_properties());
            new_trans_props.inherit(trans_props);
            trList.append(cp);
            field->disconnect_service(transition);
        }
        //else kDebug() << "// FOUND TRANS OK, "<<resource<< ", A_: " << aTrack << ", B_ "<<bTrack;

        nextservice = mlt_service_producer(nextservice);
        if (nextservice == NULL) break;
        properties = MLT_SERVICE_PROPERTIES(nextservice);
        mlt_type = mlt_properties_get(properties, "mlt_type");
        resource = mlt_properties_get(properties, "mlt_service");
    }
    field->plant_transition(tr, a_track, b_track);

    // re-add upper transitions
    for (int i = trList.count() - 1; i >= 0; i--) {
        //kDebug()<< "REPLANT ON TK: "<<trList.at(i)->get_a_track()<<", "<<trList.at(i)->get_b_track();
        field->plant_transition(*trList.at(i), trList.at(i)->get_a_track(), trList.at(i)->get_b_track());
    }
}

void Render::mltUpdateTransition(QString oldTag, QString tag, int a_track, int b_track, GenTime in, GenTime out, QDomElement xml, bool force)
{
    if (oldTag == tag && !force) mltUpdateTransitionParams(tag, a_track, b_track, in, out, xml);
    else {
        //kDebug()<<"// DELETING TRANS: "<<a_track<<"-"<<b_track;
        mltDeleteTransition(oldTag, a_track, b_track, in, out, xml, false);
        mltAddTransition(tag, a_track, b_track, in, out, xml, false);
    }

    if (m_mltProducer->position() >= in.frames(m_fps) && m_mltProducer->position() <= out.frames(m_fps)) refresh();
}

void Render::mltUpdateTransitionParams(QString type, int a_track, int b_track, GenTime in, GenTime out, QDomElement xml)
{
    mlt_service serv = m_mltProducer->parent().get_service();
    mlt_service_lock(serv);
    m_isBlocked++;

    mlt_service nextservice = mlt_service_get_producer(serv);
    mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
    QString mlt_type = mlt_properties_get(properties, "mlt_type");
    QString resource = mlt_properties_get(properties, "mlt_service");
    int in_pos = (int) in.frames(m_fps);
    int out_pos = (int) out.frames(m_fps) - 1;

    while (mlt_type == "transition") {
        mlt_transition tr = (mlt_transition) nextservice;
        int currentTrack = mlt_transition_get_b_track(tr);
        int currentBTrack = mlt_transition_get_a_track(tr);
        int currentIn = (int) mlt_transition_get_in(tr);
        int currentOut = (int) mlt_transition_get_out(tr);

        // kDebug()<<"Looking for transition : " << currentIn <<'x'<<currentOut<< ", OLD oNE: "<<in_pos<<'x'<<out_pos;
        if (resource == type && b_track == currentTrack && currentIn == in_pos && currentOut == out_pos) {
            QMap<QString, QString> map = mltGetTransitionParamsFromXml(xml);
            QMap<QString, QString>::Iterator it;
            QString key;
            mlt_properties transproperties = MLT_TRANSITION_PROPERTIES(tr);

            QString currentId = mlt_properties_get(transproperties, "kdenlive_id");
            if (currentId != xml.attribute("id")) {
                // The transition ID is not the same, so reset all properties
                mlt_properties_set(transproperties, "kdenlive_id", xml.attribute("id").toUtf8().constData());
                // Cleanup previous properties
                QStringList permanentProps;
                permanentProps << "factory" << "kdenlive_id" << "mlt_service" << "mlt_type" << "in";
                permanentProps << "out" << "a_track" << "b_track";
                for (int i = 0; i < mlt_properties_count(transproperties); i++) {
                    QString propName = mlt_properties_get_name(transproperties, i);
                    if (!propName.startsWith('_') && ! permanentProps.contains(propName)) {
                        mlt_properties_set(transproperties, propName.toUtf8().constData(), "");
                    }
                }
            }

            mlt_properties_set_int(transproperties, "force_track", xml.attribute("force_track").toInt());
            mlt_properties_set_int(transproperties, "automatic", xml.attribute("automatic", "0").toInt());

            if (currentBTrack != a_track) {
                mlt_properties_set_int(transproperties, "a_track", a_track);
            }
            for (it = map.begin(); it != map.end(); ++it) {
                key = it.key();
                mlt_properties_set(transproperties, key.toUtf8().constData(), it.value().toUtf8().constData());
                //kDebug() << " ------  UPDATING TRANS PARAM: " << key.toUtf8().constData() << ": " << it.value().toUtf8().constData();
                //filter->set("kdenlive_id", id);
            }
            break;
        }
        nextservice = mlt_service_producer(nextservice);
        if (nextservice == NULL) break;
        properties = MLT_SERVICE_PROPERTIES(nextservice);
        mlt_type = mlt_properties_get(properties, "mlt_type");
        resource = mlt_properties_get(properties, "mlt_service");
    }
    mlt_service_unlock(serv);
    m_isBlocked--;
    //askForRefresh();
    //if (m_isBlocked == 0) m_mltConsumer->set("refresh", 1);
}

void Render::mltDeleteTransition(QString tag, int /*a_track*/, int b_track, GenTime in, GenTime out, QDomElement /*xml*/, bool /*do_refresh*/)
{
    mlt_service serv = m_mltProducer->parent().get_service();
    m_isBlocked++;
    mlt_service_lock(serv);

    Mlt::Service service(serv);
    Mlt::Tractor tractor(service);
    Mlt::Field *field = tractor.field();

    //if (do_refresh) m_mltConsumer->set("refresh", 0);

    mlt_service nextservice = mlt_service_get_producer(serv);
    mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
    QString mlt_type = mlt_properties_get(properties, "mlt_type");
    QString resource = mlt_properties_get(properties, "mlt_service");

    const int old_pos = (int)((in + out).frames(m_fps) / 2);
    //kDebug() << " del trans pos: " << in.frames(25) << "-" << out.frames(25);

    while (mlt_type == "transition") {
        mlt_transition tr = (mlt_transition) nextservice;
        int currentTrack = mlt_transition_get_b_track(tr);
        int currentIn = (int) mlt_transition_get_in(tr);
        int currentOut = (int) mlt_transition_get_out(tr);
        //kDebug() << "// FOUND EXISTING TRANS, IN: " << currentIn << ", OUT: " << currentOut << ", TRACK: " << currentTrack;

        if (resource == tag && b_track == currentTrack && currentIn <= old_pos && currentOut >= old_pos) {
            mlt_field_disconnect_service(field->get_field(), nextservice);
            break;
        }
        nextservice = mlt_service_producer(nextservice);
        if (nextservice == NULL) break;
        properties = MLT_SERVICE_PROPERTIES(nextservice);
        mlt_type = mlt_properties_get(properties, "mlt_type");
        resource = mlt_properties_get(properties, "mlt_service");
    }
    mlt_service_unlock(serv);
    m_isBlocked--;
    //askForRefresh();
    //if (m_isBlocked == 0) m_mltConsumer->set("refresh", 1);
}

QMap<QString, QString> Render::mltGetTransitionParamsFromXml(QDomElement xml)
{
    QDomNodeList attribs = xml.elementsByTagName("parameter");
    QMap<QString, QString> map;
    for (int i = 0; i < attribs.count(); i++) {
        QDomElement e = attribs.item(i).toElement();
        QString name = e.attribute("name");
        //kDebug()<<"-- TRANSITION PARAM: "<<name<<" = "<< e.attribute("name")<<" / " << e.attribute("value");
        map[name] = e.attribute("default");
        if (!e.attribute("value").isEmpty()) {
            map[name] = e.attribute("value");
        }
        if (e.attribute("type") != "addedgeometry" && !e.attribute("factor").isEmpty() && e.attribute("factor").toDouble() > 0) {
            map[name] = m_locale.toString(map.value(name).toDouble() / e.attribute("factor").toDouble());
            //map[name]=map[name].replace(".",","); //FIXME how to solve locale conversion of . ,
        }

        if (e.attribute("namedesc").contains(';')) {
            QString format = e.attribute("format");
            QStringList separators = format.split("%d", QString::SkipEmptyParts);
            QStringList values = e.attribute("value").split(QRegExp("[,:;x]"));
            QString neu;
            QTextStream txtNeu(&neu);
            if (values.size() > 0)
                txtNeu << (int)values[0].toDouble();
            int i = 0;
            for (i = 0; i < separators.size() && i + 1 < values.size(); i++) {
                txtNeu << separators[i];
                txtNeu << (int)(values[i+1].toDouble());
            }
            if (i < separators.size())
                txtNeu << separators[i];
            map[e.attribute("name")] = neu;
        }

    }
    return map;
}

void Render::mltAddClipTransparency(ItemInfo info, int transitiontrack, int id)
{
    kDebug() << "/////////  ADDING CLIP TRANSPARENCY AT: " << info.startPos.frames(25);
    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);
    Mlt::Field *field = tractor.field();

    Mlt::Transition *transition = new Mlt::Transition(*m_mltProfile, "composite");
    transition->set_in_and_out((int) info.startPos.frames(m_fps), (int) info.endPos.frames(m_fps) - 1);
    transition->set("transparency", id);
    transition->set("fill", 1);
    transition->set("internal_added", 237);
    field->plant_transition(*transition, transitiontrack, info.track);
    refresh();
}

void Render::mltDeleteTransparency(int pos, int track, int id)
{
    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);
    Mlt::Field *field = tractor.field();

    //if (do_refresh) m_mltConsumer->set("refresh", 0);
    mlt_service serv = m_mltProducer->parent().get_service();

    mlt_service nextservice = mlt_service_get_producer(serv);
    mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
    QString mlt_type = mlt_properties_get(properties, "mlt_type");
    QString resource = mlt_properties_get(properties, "mlt_service");

    while (mlt_type == "transition") {
        mlt_transition tr = (mlt_transition) nextservice;
        int currentTrack = mlt_transition_get_b_track(tr);
        int currentIn = (int) mlt_transition_get_in(tr);
        int currentOut = (int) mlt_transition_get_out(tr);
        int transitionId = QString(mlt_properties_get(properties, "transparency")).toInt();
        kDebug() << "// FOUND EXISTING TRANS, IN: " << currentIn << ", OUT: " << currentOut << ", TRACK: " << currentTrack;

        if (resource == "composite" && track == currentTrack && currentIn == pos && transitionId == id) {
            //kDebug() << " / / / / /DELETE TRANS DOOOMNE";
            mlt_field_disconnect_service(field->get_field(), nextservice);
            break;
        }
        nextservice = mlt_service_producer(nextservice);
        if (nextservice == NULL) break;
        properties = MLT_SERVICE_PROPERTIES(nextservice);
        mlt_type = mlt_properties_get(properties, "mlt_type");
        resource = mlt_properties_get(properties, "mlt_service");
    }
    //if (do_refresh) m_mltConsumer->set("refresh", 1);
}

void Render::mltResizeTransparency(int oldStart, int newStart, int newEnd, int track, int id)
{
    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);

    mlt_service_lock(service.get_service());
    m_mltConsumer->set("refresh", 0);
    m_isBlocked++;

    mlt_service serv = m_mltProducer->parent().get_service();
    mlt_service nextservice = mlt_service_get_producer(serv);
    mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
    QString mlt_type = mlt_properties_get(properties, "mlt_type");
    QString resource = mlt_properties_get(properties, "mlt_service");
    kDebug() << "// resize transpar from: " << oldStart << ", TO: " << newStart << 'x' << newEnd << ", " << track << ", " << id;
    while (mlt_type == "transition") {
        mlt_transition tr = (mlt_transition) nextservice;
        int currentTrack = mlt_transition_get_b_track(tr);
        int currentIn = (int) mlt_transition_get_in(tr);
        //mlt_properties props = MLT_TRANSITION_PROPERTIES(tr);
        int transitionId = QString(mlt_properties_get(properties, "transparency")).toInt();
        kDebug() << "// resize transpar current in: " << currentIn << ", Track: " << currentTrack << ", id: " << id << 'x' << transitionId ;
        if (resource == "composite" && track == currentTrack && currentIn == oldStart && transitionId == id) {
            kDebug() << " / / / / /RESIZE TRANS TO: " << newStart << 'x' << newEnd;
            mlt_transition_set_in_and_out(tr, newStart, newEnd);
            break;
        }
        nextservice = mlt_service_producer(nextservice);
        if (nextservice == NULL) break;
        properties = MLT_SERVICE_PROPERTIES(nextservice);
        mlt_type = mlt_properties_get(properties, "mlt_type");
        resource = mlt_properties_get(properties, "mlt_service");
    }
    mlt_service_unlock(service.get_service());
    m_isBlocked--;
    if (m_isBlocked == 0) m_mltConsumer->set("refresh", 1);

}

void Render::mltMoveTransparency(int startTime, int endTime, int startTrack, int endTrack, int id)
{
    Mlt::Service service(m_mltProducer->parent().get_service());
    Mlt::Tractor tractor(service);

    mlt_service_lock(service.get_service());
    m_mltConsumer->set("refresh", 0);
    m_isBlocked++;

    mlt_service serv = m_mltProducer->parent().get_service();
    mlt_service nextservice = mlt_service_get_producer(serv);
    mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
    QString mlt_type = mlt_properties_get(properties, "mlt_type");
    QString resource = mlt_properties_get(properties, "mlt_service");

    while (mlt_type == "transition") {
        mlt_transition tr = (mlt_transition) nextservice;
        int currentTrack = mlt_transition_get_b_track(tr);
        int currentaTrack = mlt_transition_get_a_track(tr);
        int currentIn = (int) mlt_transition_get_in(tr);
        int currentOut = (int) mlt_transition_get_out(tr);
        //mlt_properties properties = MLT_TRANSITION_PROPERTIES(tr);
        int transitionId = QString(mlt_properties_get(properties, "transparency")).toInt();
        //kDebug()<<" + TRANSITION "<<id<<" == "<<transitionId<<", START TMIE: "<<currentIn<<", LOOK FR: "<<startTime<<", TRACK: "<<currentTrack<<'x'<<startTrack;
        if (resource == "composite" && transitionId == id && startTime == currentIn && startTrack == currentTrack) {
            kDebug() << "//////MOVING";
            mlt_transition_set_in_and_out(tr, endTime, endTime + currentOut - currentIn);
            if (endTrack != startTrack) {
                mlt_properties properties = MLT_TRANSITION_PROPERTIES(tr);
                mlt_properties_set_int(properties, "a_track", currentaTrack + endTrack - currentTrack);
                mlt_properties_set_int(properties, "b_track", endTrack);
            }
            break;
        }
        nextservice = mlt_service_producer(nextservice);
        if (nextservice == NULL) break;
        properties = MLT_SERVICE_PROPERTIES(nextservice);
        mlt_type = mlt_properties_get(properties, "mlt_type");
        resource = mlt_properties_get(properties, "mlt_service");
    }
    m_isBlocked--;
    mlt_service_unlock(service.get_service());
    m_mltConsumer->set("refresh", 1);
}


bool Render::mltAddTransition(QString tag, int a_track, int b_track, GenTime in, GenTime out, QDomElement xml, bool do_refresh)
{
    if (in >= out) return false;
    QMap<QString, QString> args = mltGetTransitionParamsFromXml(xml);
    Mlt::Service service(m_mltProducer->parent().get_service());

    Mlt::Tractor tractor(service);
    Mlt::Field *field = tractor.field();

    Mlt::Transition *transition = new Mlt::Transition(*m_mltProfile, tag.toUtf8().constData());
    if (out != GenTime())
        transition->set_in_and_out((int) in.frames(m_fps), (int) out.frames(m_fps) - 1);

    if (do_refresh && (m_mltProducer->position() < in.frames(m_fps) || m_mltProducer->position() > out.frames(m_fps))) do_refresh = false;
    QMap<QString, QString>::Iterator it;
    QString key;
    if (xml.attribute("automatic") == "1") transition->set("automatic", 1);
    //kDebug() << " ------  ADDING TRANSITION PARAMs: " << args.count();
    if (xml.hasAttribute("id"))
        transition->set("kdenlive_id", xml.attribute("id").toUtf8().constData());
    if (xml.hasAttribute("force_track"))
        transition->set("force_track", xml.attribute("force_track").toInt());

    for (it = args.begin(); it != args.end(); ++it) {
        key = it.key();
        if (!it.value().isEmpty())
            transition->set(key.toUtf8().constData(), it.value().toUtf8().constData());
        //kDebug() << " ------  ADDING TRANS PARAM: " << key << ": " << it.value();
    }
    // attach transition
    m_isBlocked++;
    mlt_service_lock(service.get_service());
    mltPlantTransition(field, *transition, a_track, b_track);
    // field->plant_transition(*transition, a_track, b_track);
    mlt_service_unlock(service.get_service());
    m_isBlocked--;
    if (do_refresh) refresh();
    return true;
}

void Render::mltSavePlaylist()
{
    kWarning() << "// UPDATING PLAYLIST TO DISK++++++++++++++++";
    Mlt::Consumer fileConsumer(*m_mltProfile, "xml");
    fileConsumer.set("resource", "/tmp/playlist.mlt");

    Mlt::Service service(m_mltProducer->get_service());

    fileConsumer.connect(service);
    fileConsumer.start();
}

const QList <Mlt::Producer *> Render::producersList()
{
    QList <Mlt::Producer *> prods;
    if (m_mltProducer == NULL) return prods;
    Mlt::Service service(m_mltProducer->parent().get_service());
    if (service.type() != tractor_type) return prods;
    Mlt::Tractor tractor(service);
    QStringList ids;

    int trackNb = tractor.count();
    for (int t = 1; t < trackNb; t++) {
        Mlt::Producer *tt = tractor.track(t);
        Mlt::Producer trackProducer(tt);
        delete tt;
        Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
        int clipNb = trackPlaylist.count();
        for (int i = 0; i < clipNb; i++) {
            Mlt::Producer *c = trackPlaylist.get_clip(i);
            if (c == NULL) continue;
            QString prodId = c->parent().get("id");
            if (!c->is_blank() && !ids.contains(prodId) && !prodId.startsWith("slowmotion") && !prodId.isEmpty()) {
                Mlt::Producer *nprod = new Mlt::Producer(c->get_parent());
                if (nprod) {
                    ids.append(prodId);
                    prods.append(nprod);
                }
            }
            delete c;
        }
    }
    return prods;
}

void Render::fillSlowMotionProducers()
{
    if (m_mltProducer == NULL) return;
    Mlt::Service service(m_mltProducer->parent().get_service());
    if (service.type() != tractor_type) return;

    Mlt::Tractor tractor(service);

    int trackNb = tractor.count();
    for (int t = 1; t < trackNb; t++) {
        Mlt::Producer *tt = tractor.track(t);
        Mlt::Producer trackProducer(tt);
        delete tt;
        Mlt::Playlist trackPlaylist((mlt_playlist) trackProducer.get_service());
        int clipNb = trackPlaylist.count();
        for (int i = 0; i < clipNb; i++) {
            Mlt::Producer *c = trackPlaylist.get_clip(i);
            Mlt::Producer *nprod = new Mlt::Producer(c->get_parent());
            if (nprod) {
                QString id = nprod->parent().get("id");
                if (id.startsWith("slowmotion:") && !nprod->is_blank()) {
                    // this is a slowmotion producer, add it to the list
                    QString url = QString::fromUtf8(nprod->get("resource"));
                    int strobe = nprod->get_int("strobe");
                    if (strobe > 1) url.append("&strobe=" + QString::number(strobe));
                    if (!m_slowmotionProducers.contains(url)) {
                        m_slowmotionProducers.insert(url, nprod);
                    }
                } else delete nprod;
            }
            delete c;
        }
    }
}

void Render::mltInsertTrack(int ix, bool videoTrack)
{
    blockSignals(true);
    m_isBlocked++;

    Mlt::Service service(m_mltProducer->parent().get_service());
    mlt_service_lock(service.get_service());
    if (service.type() != tractor_type) {
        kWarning() << "// TRACTOR PROBLEM";
        return;
    }

    Mlt::Tractor tractor(service);

    Mlt::Playlist playlist;
    int ct = tractor.count();
    if (ix > ct) {
        kDebug() << "// ERROR, TRYING TO insert TRACK " << ix << ", max: " << ct;
        ix = ct;
    }

    int pos = ix;
    if (pos < ct) {
        Mlt::Producer *prodToMove = new Mlt::Producer(tractor.track(pos));
        tractor.set_track(playlist, pos);
        Mlt::Producer newProd(tractor.track(pos));
        if (!videoTrack) newProd.set("hide", 1);
        pos++;
        for (; pos <= ct; pos++) {
            Mlt::Producer *prodToMove2 = new Mlt::Producer(tractor.track(pos));
            tractor.set_track(*prodToMove, pos);
            prodToMove = prodToMove2;
        }
    } else {
        tractor.set_track(playlist, ix);
        Mlt::Producer newProd(tractor.track(ix));
        if (!videoTrack) newProd.set("hide", 1);
    }

    // Move transitions
    mlt_service serv = m_mltProducer->parent().get_service();
    mlt_service nextservice = mlt_service_get_producer(serv);
    mlt_properties properties = MLT_SERVICE_PROPERTIES(nextservice);
    QString mlt_type = mlt_properties_get(properties, "mlt_type");
    QString resource = mlt_properties_get(properties, "mlt_service");

    while (mlt_type == "transition") {
        if (resource != "mix") {
            mlt_transition tr = (mlt_transition) nextservice;
            int currentTrack = mlt_transition_get_b_track(tr);
            int currentaTrack = mlt_transition_get_a_track(tr);
            mlt_properties properties = MLT_TRANSITION_PROPERTIES(tr);

            if (currentTrack >= ix) {
                mlt_properties_set_int(properties, "b_track", currentTrack + 1);
                mlt_properties_set_int(properties, "a_track", currentaTrack + 1);
            }
        }
        nextservice = mlt_service_producer(nextservice);
        if (nextservice == NULL) break;
        properties = MLT_SERVICE_PROPERTIES(nextservice);
        mlt_type = mlt_properties_get(properties, "mlt_type");
        resource = mlt_properties_get(properties, "mlt_service");
    }

    // Add audio mix transition to last track
    Mlt::Field *field = tractor.field();
    Mlt::Transition *transition = new Mlt::Transition(*m_mltProfile, "mix");
    transition->set("a_track", 1);
    transition->set("b_track", ct);
    transition->set("always_active", 1);
    transition->set("internal_added", 237);
    transition->set("combine", 1);
    field->plant_transition(*transition, 1, ct);
    //mlt_service_unlock(m_mltConsumer->get_service());
    mlt_service_unlock(service.get_service());
    //tractor.multitrack()->refresh();
    //tractor.refresh();
    m_isBlocked--;
    blockSignals(false);
}


void Render::mltDeleteTrack(int ix)
{
    QDomDocument doc;
    doc.setContent(sceneList(), false);
    int tracksCount = doc.elementsByTagName("track").count() - 1;
    QDomNode track = doc.elementsByTagName("track").at(ix);
    QDomNode tractor = doc.elementsByTagName("tractor").at(0);
    QDomNodeList transitions = doc.elementsByTagName("transition");
    for (int i = 0; i < transitions.count(); i++) {
        QDomElement e = transitions.at(i).toElement();
        QDomNodeList props = e.elementsByTagName("property");
        QMap <QString, QString> mappedProps;
        for (int j = 0; j < props.count(); j++) {
            QDomElement f = props.at(j).toElement();
            mappedProps.insert(f.attribute("name"), f.firstChild().nodeValue());
        }
        if (mappedProps.value("mlt_service") == "mix" && mappedProps.value("b_track").toInt() == tracksCount) {
            tractor.removeChild(transitions.at(i));
            i--;
        } else if (mappedProps.value("mlt_service") != "mix" && (mappedProps.value("b_track").toInt() >= ix || mappedProps.value("a_track").toInt() >= ix)) {
            // Transition needs to be moved
            int a_track = mappedProps.value("a_track").toInt();
            int b_track = mappedProps.value("b_track").toInt();
            if (a_track > 0 && a_track >= ix) a_track --;
            if (b_track == ix) {
                // transition was on the deleted track, so remove it
                tractor.removeChild(transitions.at(i));
                i--;
                continue;
            }
            if (b_track > 0 && b_track > ix) b_track --;
            for (int j = 0; j < props.count(); j++) {
                QDomElement f = props.at(j).toElement();
                if (f.attribute("name") == "a_track") f.firstChild().setNodeValue(QString::number(a_track));
                else if (f.attribute("name") == "b_track") f.firstChild().setNodeValue(QString::number(b_track));
            }

        }
    }
    tractor.removeChild(track);
    //kDebug() << "/////////// RESULT SCENE: \n" << doc.toString();
    setSceneList(doc.toString(), m_framePosition);
    emit refreshDocumentProducers(false, false);
}


void Render::updatePreviewSettings()
{
    kDebug() << "////// RESTARTING CONSUMER";
    if (!m_mltConsumer || !m_mltProducer) return;
    if (m_mltProducer->get_playtime() == 0) return;
    Mlt::Service service(m_mltProducer->parent().get_service());
    if (service.type() != tractor_type) return;

    //m_mltConsumer->set("refresh", 0);
    if (!m_mltConsumer->is_stopped()) m_mltConsumer->stop();
    m_mltConsumer->purge();
    QString scene = sceneList();
    int pos = 0;
    if (m_mltProducer) {
        pos = m_mltProducer->position();
    }

    setSceneList(scene, pos);
}


QString Render::updateSceneListFps(double current_fps, double new_fps, QString scene)
{
    // Update all frame positions to the new fps value
    //WARNING: there are probably some effects or other that hold a frame value
    // as parameter and will also need to be updated here!
    QDomDocument doc;
    doc.setContent(scene);

    double factor = new_fps / current_fps;
    QDomNodeList producers = doc.elementsByTagName("producer");
    for (int i = 0; i < producers.count(); i++) {
        QDomElement prod = producers.at(i).toElement();
        prod.removeAttribute("in");
        prod.removeAttribute("out");

        QDomNodeList props = prod.childNodes();
        for (int j = 0; j < props.count(); j++) {
            QDomElement param =  props.at(j).toElement();
            QString paramName = param.attribute("name");
            if (paramName.startsWith("meta.") || paramName == "length") {
                prod.removeChild(props.at(j));
                j--;
            }
        }
    }

    QDomNodeList entries = doc.elementsByTagName("entry");
    for (int i = 0; i < entries.count(); i++) {
        QDomElement entry = entries.at(i).toElement();
        int in = entry.attribute("in").toInt();
        int out = entry.attribute("out").toInt();
        in = factor * in + 0.5;
        out = factor * out + 0.5;
        entry.setAttribute("in", in);
        entry.setAttribute("out", out);
    }

    QDomNodeList blanks = doc.elementsByTagName("blank");
    for (int i = 0; i < blanks.count(); i++) {
        QDomElement blank = blanks.at(i).toElement();
        int length = blank.attribute("length").toInt();
        length = factor * length + 0.5;
        blank.setAttribute("length", QString::number(length));
    }

    QDomNodeList filters = doc.elementsByTagName("filter");
    for (int i = 0; i < filters.count(); i++) {
        QDomElement filter = filters.at(i).toElement();
        int in = filter.attribute("in").toInt();
        int out = filter.attribute("out").toInt();
        in = factor * in + 0.5;
        out = factor * out + 0.5;
        filter.setAttribute("in", in);
        filter.setAttribute("out", out);
    }

    QDomNodeList transitions = doc.elementsByTagName("transition");
    for (int i = 0; i < transitions.count(); i++) {
        QDomElement transition = transitions.at(i).toElement();
        int in = transition.attribute("in").toInt();
        int out = transition.attribute("out").toInt();
        in = factor * in + 0.5;
        out = factor * out + 0.5;
        transition.setAttribute("in", in);
        transition.setAttribute("out", out);
        QDomNodeList props = transition.childNodes();
        for (int j = 0; j < props.count(); j++) {
            QDomElement param =  props.at(j).toElement();
            QString paramName = param.attribute("name");
            if (paramName == "geometry") {
                QString geom = param.firstChild().nodeValue();
                QStringList keys = geom.split(';');
                QStringList newKeys;
                for (int k = 0; k < keys.size(); ++k) {
                    if (keys.at(k).contains('=')) {
                        int pos = keys.at(k).section('=', 0, 0).toInt();
                        pos = factor * pos + 0.5;
                        newKeys.append(QString::number(pos) + '=' + keys.at(k).section('=', 1));
                    } else newKeys.append(keys.at(k));
                }
                param.firstChild().setNodeValue(newKeys.join(";"));
            }
        }
    }
    QDomElement tractor = doc.elementsByTagName("tractor").at(0).toElement();
    int out = tractor.attribute("out").toInt();
    out = factor * out + 0.5;
    tractor.setAttribute("out", out);
    emit durationChanged(out);

    //kDebug() << "///////////////////////////// " << out << " \n" << doc.toString() << "\n-------------------------";
    return doc.toString();
}


void Render::sendFrameUpdate()
{
    if (m_mltProducer) {
        Mlt::Frame * frame = m_mltProducer->get_frame();
        emitFrameUpdated(*frame);
        delete frame;
    }
}

Mlt::Producer* Render::getProducer()
{
    return m_mltProducer;
}


#include "renderer.moc"

