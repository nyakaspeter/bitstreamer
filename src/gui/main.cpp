#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>

#include <cstring>

#include "MainWindow.h"

namespace {

// The icon is drawn at run time so the repository needs no binary assets.
QIcon makeIcon() {
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(28, 36, 52));
        p.drawRoundedRect(QRectF(0, 0, size, size), size * 0.22, size * 0.22);
        // Six bars: one per 5.1 channel.
        static const double heights[6] = {0.55, 0.8, 0.45, 0.7, 0.35, 0.6};
        const double margin = size * 0.18;
        const double w = (size - 2 * margin) / 6.0;
        p.setBrush(QColor(96, 205, 255));
        for (int i = 0; i < 6; ++i) {
            const double h = (size - 2 * margin) * heights[i];
            p.drawRoundedRect(QRectF(margin + i * w + w * 0.18, size - margin - h, w * 0.64, h), w * 0.3, w * 0.3);
        }
        icon.addPixmap(pm);
    }
    return icon;
}

// The input device is Valve's "Steam Streaming Speakers". If it is missing it
// is installed from the driver package that ships with Steam (after asking;
// Windows shows its administrator prompt). Returns false if the user quits.
bool ensureSteamSpeakers() {
    for (;;) {
        if (!bs::findSteamSpeakers().id.empty()) return true;

        if (bs::steamSpeakersDriverInf().empty()) {
            QMessageBox box(QMessageBox::Warning, QStringLiteral("bitstreamer"),
                            QObject::tr("bitstreamer needs Steam, which provides the virtual sound device it captures "
                                        "from.\n\nInstall Steam, then choose Retry."));
            box.setStandardButtons(QMessageBox::Retry | QMessageBox::Close);
            box.button(QMessageBox::Close)->setText(QObject::tr("Quit"));
            if (box.exec() != QMessageBox::Retry) return false;
            continue;
        }

        QMessageBox ask(QMessageBox::Question, QStringLiteral("bitstreamer"),
                        QObject::tr("bitstreamer needs the \"Steam Streaming Speakers\" virtual sound device, which is not "
                                    "installed yet."));
        QPushButton* install = ask.addButton(QObject::tr("Install"), QMessageBox::AcceptRole);
        ask.addButton(QObject::tr("Quit"), QMessageBox::RejectRole);
        ask.exec();
        if (ask.clickedButton() != install) return false;

        std::string error;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bs::SteamInstallResult result = bs::installSteamSpeakersElevated(
            QCoreApplication::applicationFilePath().replace('/', '\\').toStdWString(), L"--install-steam-speakers", error);
        QApplication::restoreOverrideCursor();
        if (result == bs::SteamInstallResult::Installed) return true;

        QMessageBox failed(QMessageBox::Warning, QStringLiteral("bitstreamer"),
                           QObject::tr("The device could not be installed: %1.").arg(QString::fromStdString(error)));
        failed.setStandardButtons(QMessageBox::Retry | QMessageBox::Close);
        failed.button(QMessageBox::Close)->setText(QObject::tr("Quit"));
        if (failed.exec() != QMessageBox::Retry) return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Elevated helper mode: install the driver and report the Win32 result.
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--install-steam-speakers") == 0) return static_cast<int>(bs::installSteamSpeakersDriver());

    // Only one instance may run: a second launch brings the first to the front.
    const HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\bitstreamer.single-instance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        PostMessageW(HWND_BROADCAST, MainWindow::showMessageId(), 0, 0);
        return 0;
    }

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("bitstreamer"));
    QCoreApplication::setApplicationName(QStringLiteral("bitstreamer"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BITSTREAMER_VERSION));
    QApplication::setWindowIcon(makeIcon());
    QApplication::setQuitOnLastWindowClosed(false);

    bs::ComInit com;
    const QStringList args = app.arguments();
    const bool minimized = args.contains(QStringLiteral("--minimized"));
    if (!ensureSteamSpeakers()) return 0;
    MainWindow window(minimized);

    const int rc = app.exec();
    if (instanceMutex) CloseHandle(instanceMutex);
    return rc;
}
