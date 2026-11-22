// Six-channel level meter: slim rounded bars with smooth release, drawn in
// the system accent colour.
#pragma once

#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>

class LevelMeter : public QWidget {
public:
    explicit LevelMeter(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(150);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        timer_.setInterval(16);
        connect(&timer_, &QTimer::timeout, this, [this] { step(); });
    }

    // Linear peak levels in canonical order L R C LFE LS RS.
    void setLevels(const std::array<float, 8>& linearPeaks) {
        for (int c = 0; c < 6; ++c) {
            const float lin = linearPeaks[static_cast<size_t>(c)];
            const double db = lin > 1e-6f ? 20.0 * std::log10(static_cast<double>(lin)) : -120.0;
            target_[static_cast<size_t>(c)] = std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
        }
        if (!timer_.isActive()) timer_.start();
    }
    void clear() {
        target_.fill(0.0);
        if (!timer_.isActive()) timer_.start();
    }
    // A short status text shown in the middle of the meter area (empty = none).
    // It is drawn over the bars, so nothing in the window moves or resizes.
    void setMessage(const QString& text) {
        if (text == message_) return;
        message_ = text;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        static const char* names[6] = {"L", "R", "C", "LFE", "LS", "RS"};
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QPalette& pal = palette();
        QColor track = pal.color(QPalette::WindowText);
        track.setAlphaF(message_.isEmpty() ? 0.08f : 0.035f);  // the bars step back behind a message
        QColor muted = pal.color(QPalette::WindowText);
        muted.setAlphaF(0.55f);
        const QColor accent = pal.color(QPalette::Accent);

        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.9);
        p.setFont(f);
        const int labelH = QFontMetrics(f).height() + 8;
        const double barW = std::min(22.0, width() / 6.0 * 0.42);
        const double slot = width() / 6.0;
        const double barH = height() - labelH;
        for (int c = 0; c < 6; ++c) {
            const double x = slot * c + (slot - barW) / 2.0;
            const QRectF trackRect(x, 0, barW, barH);
            QPainterPath trackPath;
            trackPath.addRoundedRect(trackRect, barW / 2, barW / 2);
            p.fillPath(trackPath, track);

            const double level = shown_[static_cast<size_t>(c)];
            if (level > 0.004) {
                const double h = std::max(barW, level * barH);
                QPainterPath fill;
                fill.addRoundedRect(QRectF(x, barH - h, barW, h), barW / 2, barW / 2);
                // Close to full scale the bar warms towards amber and red.
                QColor col = accent;
                if (level > 0.985) col = QColor(232, 72, 85);
                else if (level > 0.9) col = QColor(240, 173, 78);
                p.fillPath(fill, col);
            }
            p.setPen(muted);
            p.drawText(QRectF(slot * c, barH, slot, labelH), Qt::AlignCenter, QString::fromLatin1(names[c]));
        }
        if (!message_.isEmpty()) {
            p.setFont(font());
            QColor text = pal.color(QPalette::WindowText);
            text.setAlphaF(0.8f);
            p.setPen(text);
            p.drawText(QRectF(8, 0, width() - 16, barH), Qt::AlignCenter | Qt::TextWordWrap, message_);
        }
    }

private:
    void step() {
        bool moving = false;
        for (size_t c = 0; c < 6; ++c) {
            // Instant attack, smooth release.
            const double next = target_[c] > shown_[c] ? target_[c] : shown_[c] + (target_[c] - shown_[c]) * 0.18;
            if (std::fabs(next - shown_[c]) > 0.0005) moving = true;
            shown_[c] = std::fabs(next) < 0.0005 ? 0.0 : next;
        }
        update();
        if (!moving) timer_.stop();
    }

    QTimer timer_;
    std::array<double, 6> target_{};
    std::array<double, 6> shown_{};
    QString message_;
};
