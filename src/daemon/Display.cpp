/***************************************************************************
* Copyright (c) 2014 Pier Luigi Fiorini <pierluigi.fiorini@gmail.com>
* Copyright (c) 2014 Martin Bříza <mbriza@redhat.com>
* Copyright (c) 2013 Abdurrahman AVCI <abdurrahmanavci@gmail.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the
* Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
***************************************************************************/

#include "Display.h"

#include "Configuration.h"
#include "DaemonApp.h"
#include "DisplayManager.h"
#include "DisplayServer.h"
#include "Seat.h"
#include "SocketServer.h"
#include "Greeter.h"
#include "Utils.h"
#include "SignalHandler.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTimer>

#include <grp.h>
#include <pwd.h>
#include <unistd.h>

namespace SDDM {
    Display::Display(const int displayId, const int terminalId, Seat *parent) : QObject(parent),
        m_displayId(displayId), m_terminalId(terminalId),
        m_auth(new Auth(this)),
        m_displayServer(new DisplayServer(this)),
        m_seat(parent),
        m_socketServer(new SocketServer(this)),
        m_greeter(new Greeter(this)) {

        m_display = QString(":%1").arg(m_displayId);

        // respond to authentication requests
        m_auth->setVerbose(true);
        connect(m_auth, SIGNAL(requestChanged()), this, SLOT(slotRequestChanged()));
        connect(m_auth, SIGNAL(authentication(QString,bool)), this, SLOT(slotAuthenticationFinished(QString,bool)));
        connect(m_auth, SIGNAL(session(bool)), this, SLOT(slotSessionStarted(bool)));
        connect(m_auth, SIGNAL(finished(Auth::HelperExitStatus)), this, SLOT(slotHelperFinished(Auth::HelperExitStatus)));
        connect(m_auth, SIGNAL(info(QString,Auth::Info)), this, SLOT(slotAuthInfo(QString,Auth::Info)));
        connect(m_auth, SIGNAL(error(QString,Auth::Error)), this, SLOT(slotAuthError(QString,Auth::Error)));

        // restart display after display server ended
        connect(m_displayServer, SIGNAL(stopped()), this, SLOT(stop()));

        // notify the display after display server started
        connect(DaemonApp::instance()->signalHandler(), SIGNAL(sigusr1Received()), this, SLOT(displayServerStarted()));

        // connect login signal
        connect(m_socketServer, SIGNAL(login(QLocalSocket*,QString,QString,QString)), this, SLOT(login(QLocalSocket*,QString,QString,QString)));

        // connect login result signals
        connect(this, SIGNAL(loginFailed(QLocalSocket*)), m_socketServer, SLOT(loginFailed(QLocalSocket*)));
        connect(this, SIGNAL(loginSucceeded(QLocalSocket*)), m_socketServer, SLOT(loginSucceeded(QLocalSocket*)));

        // get auth dir
        QString authDir = RUNTIME_DIR;

        // use "." as authdir in test mode
        if (daemonApp->testing())
            authDir = QLatin1String(".");

        // create auth dir if not existing
        QDir().mkpath(authDir);

        // set auth path
        m_authPath = QString("%1/%2").arg(authDir).arg(m_display);
    }

    Display::~Display() {
        stop();
    }

    const int Display::displayId() const {
        return m_displayId;
    }

    const int Display::terminalId() const {
        return m_terminalId;
    }

    const QString &Display::name() const {
        return m_display;
    }

    const QString &Display::cookie() const {
        return m_cookie;
    }

    Seat *Display::seat() const {
        return m_seat;
    }

    void Display::addCookie(const QString &file) {
        // log message
        qDebug() << "Adding cookie to" << file;

        // Touch file
        QFile file_handler(file);
        file_handler.open(QIODevice::WriteOnly);
        file_handler.close();

        QString cmd = QString("%1 -f %2 -q").arg(mainConfig.XDisplay.XauthPath.get()).arg(file);

        // execute xauth
        FILE *fp = popen(qPrintable(cmd), "w");

        // check file
        if (!fp)
            return;
        fprintf(fp, "remove %s\n", qPrintable(m_display));
        fprintf(fp, "add %s . %s\n", qPrintable(m_display), qPrintable(m_cookie));
        fprintf(fp, "exit\n");
        // close pipe
        pclose(fp);
    }

    void Display::start() {
        // check flag
        if (m_started)
            return;

        // generate cookie
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);

        // resever 32 bytes
        m_cookie.reserve(32);

        // create a random hexadecimal number
        const char *digits = "0123456789abcdef";
        for (int i = 0; i < 32; ++i)
            m_cookie[i] = digits[dis(gen)];

        // generate auth file
        addCookie(m_authPath);

        // change the owner and group of the auth file to the sddm user
        struct passwd *pw = getpwnam("sddm");
        if (!pw)
            qWarning() << "Failed to find the sddm user. Owner of the auth file will not be changed.";
        else {
            if(chown(qPrintable(m_authPath), pw->pw_uid, pw->pw_gid) == -1)
                qWarning() << "Failed to change owner of the auth file.";
        }

        // set display server params
        m_displayServer->setDisplay(m_display);
        m_displayServer->setAuthPath(m_authPath);

        // start display server
        m_displayServer->start();
    }

    void Display::displayServerStarted() {
        // check flag
        if (m_started)
            return;

        // setup display
        m_displayServer->setupDisplay();

        // log message
        qDebug() << "Display server started.";

        if ((daemonApp->first || mainConfig.Autologin.Relogin.get()) &&
            !mainConfig.Autologin.User.get().isEmpty() && !mainConfig.Autologin.Session.get().isEmpty()) {
            // reset first flag
            daemonApp->first = false;

            // set flags
            m_started = true;

            // start session
            m_auth->setAutologin(true);
            startAuth(mainConfig.Autologin.User.get(), QString(), mainConfig.Autologin.Session.get());

            // return
            return;
        }

        // start socket server
        m_socketServer->start(m_display);

        if (!daemonApp->testing()) {
            // change the owner and group of the socket to avoid permission denied errors
            struct passwd *pw = getpwnam("sddm");
            if (pw) {
                if (chown(qPrintable(m_socketServer->socketAddress()), pw->pw_uid, pw->pw_gid) == -1) {
                    qWarning() << "Failed to change owner of the socket";
                    return;
                }
            }
        }

        // set greeter params
        m_greeter->setDisplay(this);
        m_greeter->setAuthPath(m_authPath);
        m_greeter->setSocket(m_socketServer->socketAddress());
        m_greeter->setTheme(QString("%1/%2").arg(mainConfig.Theme.ThemeDir.get()).arg(mainConfig.Theme.Current.get()));

        // start greeter
        m_greeter->start();

        // reset first flag
        daemonApp->first = false;

        // set flags
        m_started = true;
    }

    void Display::stop() {
        // check flag
        if (!m_started)
            return;

        // stop the greeter
        m_greeter->stop();

        // stop socket server
        m_socketServer->stop();

        // stop display server
        m_displayServer->blockSignals(true);
        m_displayServer->stop();
        m_displayServer->blockSignals(false);

        // remove authority file
        QFile::remove(m_authPath);

        // reset flag
        m_started = false;

        // emit signal
        emit stopped();
    }

    void Display::login(QLocalSocket *socket, const QString &user, const QString &password, const QString &session) {
        m_socket = socket;
        startAuth(user, password, session);
    }

    void Display::startAuth(const QString &user, const QString &password, const QString &session) {
        QString sessionName;
        QString xdgSessionName;
        QString command;

        m_passPhrase = password;

        if (session.endsWith(".desktop")) {
            // session directory
            QDir dir(mainConfig.XDisplay.SessionDir.get());

            // session file
            QFile file(dir.absoluteFilePath(session));

            // open file
            if (file.open(QIODevice::ReadOnly)) {
                // read line-by-line
                QTextStream in(&file);
                while (!in.atEnd()) {
                    QString line = in.readLine();

                    // line starting with Exec
                    if (line.startsWith("Exec="))
                        command = line.mid(5);

                    // Desktop names, change the separator
                    if (line.startsWith("DesktopNames=")) {
                        xdgSessionName = line.mid(13);
                        xdgSessionName.replace(';', ':');
                    }
                }

                // close file
                file.close();
            }

            // remove extension
            sessionName = session.left(session.lastIndexOf("."));
        } else {
            command = session;
            sessionName = session;
        }

        if (command.isEmpty()) {
            qCritical() << "Failed to find command for session:" << session;
            return;
        }

        // save session desktop file name, we'll use it to set the
        // last session later, in slotAuthenticationFinished()
        m_sessionName = session;

        QProcessEnvironment env;
        env.insert("PATH", mainConfig.Users.DefaultPath.get());
        env.insert("DISPLAY", name());
        env.insert("XDG_SEAT", seat()->name());
        env.insert("XDG_SEAT_PATH", daemonApp->displayManager()->seatPath(seat()->name()));
        env.insert("XDG_SESSION_PATH", daemonApp->displayManager()->sessionPath(QString("Session%1").arg(daemonApp->newSessionId())));
        env.insert("XDG_VTNR", QString::number(terminalId()));
        env.insert("DESKTOP_SESSION", sessionName);
        env.insert("XDG_CURRENT_DESKTOP", xdgSessionName);
        env.insert("XDG_SESSION_CLASS", "user");
        env.insert("XDG_SESSION_TYPE", "x11");
        env.insert("XDG_SESSION_DESKTOP", xdgSessionName);
        m_auth->insertEnvironment(env);

        m_auth->setUser(user);
        m_auth->setSession(command);
        m_auth->start();
    }

    void Display::slotAuthenticationFinished(const QString &user, bool success) {
        if (success) {
            qDebug() << "Authenticated successfully";

            struct passwd *pw = getpwnam(qPrintable(user));
            if (pw) {
                addCookie(QString("%1/.Xauthority").arg(pw->pw_dir));
                chown(qPrintable(QString("%1/.Xauthority").arg(pw->pw_dir)), pw->pw_uid, pw->pw_gid);
            }

            // save last user and last session
            stateConfig.Last.User.set(m_auth->user());
            stateConfig.Last.Session.set(m_sessionName);
            stateConfig.save();

            if (m_socket)
                emit loginSucceeded(m_socket);
        } else if (m_socket) {
            qDebug() << "Authentication failure";
            emit loginFailed(m_socket);
        }
        m_socket = nullptr;
    }

    void Display::slotAuthInfo(const QString &message, Auth::Info info) {
        // TODO: presentable to the user, eventually
        Q_UNUSED(info);
        qWarning() << "Authentication information:" << message;
    }

    void Display::slotAuthError(const QString &message, Auth::Error error) {
        // TODO: handle more errors
        qWarning() << "Authentication error:" << message;

        if (!m_socket)
            return;

        if (error == Auth::ERROR_AUTHENTICATION)
            emit loginFailed(m_socket);
    }

    void Display::slotHelperFinished(Auth::HelperExitStatus status) {
        // Don't restart greeter and display server unless sddm-helper exited
        // with an internal error or the user session finished successfully,
        // we want to avoid greeter from restarting when an authentication
        // error happens (in this case we want to show the message from the
        // greeter
        if (status != Auth::HELPER_AUTH_ERROR)
            stop();
    }

    void Display::slotRequestChanged() {
        if (m_auth->request()->prompts().length() == 1) {
            m_auth->request()->prompts()[0]->setResponse(qPrintable(m_passPhrase));
            m_auth->request()->done();
        } else if (m_auth->request()->prompts().length() == 2) {
            m_auth->request()->prompts()[0]->setResponse(qPrintable(m_auth->user()));
            m_auth->request()->prompts()[1]->setResponse(qPrintable(m_passPhrase));
            m_auth->request()->done();
        }
    }

    void Display::slotSessionStarted(bool success) {
        qDebug() << "Session started";
    }
}
