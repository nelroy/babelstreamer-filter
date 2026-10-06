// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  BrandedPopup.cpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//=======================================================================

#include "BrandedPopup.hpp"
#include "BrandPalette.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSizePolicy>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

/*
    Content is embedded in a namespace as the object shoud not be directly instantaible. Instead the showBabelStreamerPopup function
    should be used
 */
namespace {

constexpr const char *kLogoFilename = "babelstreamer-fish.png";

/*
  Constants to ensure style compatible with website
*/
using namespace brandpalette;

/*
 Helper to get colour for various types of popup

 @param kind   Type  of popup
 
 */
QColor accentFor(PopupKind kind)
{
	switch (kind) {
	case PopupKind::SessionCreated:
		return QColor(kAccentTeal);
	case PopupKind::UsageWarning:
		return QColor(kAccentAmber);
	case PopupKind::SessionDisconnected:
		return QColor(kAccentCoral);
	case PopupKind::NoModelSelected:
		return QColor(kAccentAmber);
	case PopupKind::UpdateAvailable:
		return QColor(kAccentTeal);
	}
	return QColor(kAccentTeal);
}

/*
    Class to generate a BabelStreamer formatted dialogue based on standard Qt dialogue. Should only be instantiated
    via showBabelStreamerPopup
 */
class BabelStreamerDialogue : public QDialog {
public:
	/*
        Constructor
     
        @param kind            type of popup dialogue (@see PopupKind)
        @param title           dialogue box title string
        @param bodyText        text for body of dialogue
        @param sessionKey      optional copyable session code in the dialogue
        @param modelURL        adds an actionable URL to download a model
        @param parent          parent widget for this dialogue
        
     */
	BabelStreamerDialogue(PopupKind kind, const QString &title, const QString &bodyText, const QString &sessionKey,
			      const QString &modelUrl, QWidget *parent)
		: QDialog(parent)
	{
		// Do the basic styled box setup
		setWindowTitle(QStringLiteral("BabelStreamer"));
		setAttribute(Qt::WA_DeleteOnClose);
		setFixedWidth(440);
		setStyleSheet(QStringLiteral("QDialog { background: %1; }").arg(kBckg));

		const QColor accent = accentFor(kind);

		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(0, 0, 0, 0);
		root->setSpacing(0);

		// Thin accent strip along the top. Colour depends on kind of popup
		auto *accentBar = new QFrame(this);
		accentBar->setFixedHeight(4);
		accentBar->setStyleSheet(QStringLiteral("background: %1; border: none;").arg(accent.name()));
		root->addWidget(accentBar);

		auto *content = new QWidget(this);
		auto *contentLayout = new QVBoxLayout(content);
		contentLayout->setContentsMargins(28, 24, 28, 24);
		contentLayout->setSpacing(14);
		root->addWidget(content);

		auto *titleLabel = new QLabel(title, content);
		QFont titleFont = titleLabel->font();
		titleFont.setBold(true);
		titleFont.setPointSize(titleFont.pointSize() + 3);
		titleLabel->setFont(titleFont);
		titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(kText));

		auto *headerRow = new QHBoxLayout();
		headerRow->addWidget(titleLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
		headerRow->addStretch(1);

		// Try and add the babelstreamer logo with graceful failure if not available
		if (char *logoPath = obs_module_file(kLogoFilename)) {
			QPixmap logo(logoPath);
			bfree(logoPath);
			if (!logo.isNull()) {
				auto *logoLabel = new QLabel(content);
				logoLabel->setPixmap(logo.scaledToHeight(32, Qt::SmoothTransformation));
				headerRow->addWidget(logoLabel, 0, Qt::AlignRight | Qt::AlignTop);
			}
		} else {
			blog(LOG_WARNING,
			     "[babelstreamer-filter] Bundled fish icon '%s' is missing from the "
			     "plugin's data directory - popup will show without it. "
			     "Reinstalling the plugin should restore it.",
			     kLogoFilename);
		}

		contentLayout->addLayout(headerRow);

		// Add the body text (if any)
		if (!bodyText.isEmpty()) {
			auto *bodyLabel = new QLabel(bodyText, content);
			bodyLabel->setWordWrap(true);
			bodyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
			bodyLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;").arg(kText));
			contentLayout->addWidget(bodyLabel);
		}

		// If there is a session Key then we need to add it with a Copy button
		if (!sessionKey.isEmpty()) {
			auto *copyLabel = new QLabel(QStringLiteral("SESSION KEY"), content);
			QFont copyLabelFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
			copyLabelFont.setPointSize(qMax(copyLabelFont.pointSize() - 1, 9));
			copyLabelFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
			copyLabel->setFont(copyLabelFont);
			copyLabel->setStyleSheet(
				QStringLiteral("color: %1; border: none; background: transparent;").arg(kMuted));
			contentLayout->addWidget(copyLabel);

			auto *monoFrame = new QFrame(content);
			monoFrame->setStyleSheet(
				QStringLiteral("QFrame { background: %1; border: 1px solid %2; border-radius: 8px; }")
					.arg(kPanel, kHairline));
			auto *monoLayout = new QHBoxLayout(monoFrame);
			monoLayout->setContentsMargins(12, 8, 8, 8);

			auto *monoLabel = new QLabel(sessionKey, monoFrame);
			monoLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
			monoLabel->setStyleSheet(
				QStringLiteral("color: %1; border: none; background: transparent;").arg(kMuted));
			monoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
			monoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
			monoLayout->addWidget(monoLabel, 1);

			auto *copyBtn = new QPushButton(QStringLiteral("Copy"), monoFrame);
			copyBtn->setCursor(Qt::PointingHandCursor);
			copyBtn->setStyleSheet(
				QStringLiteral(
					"QPushButton { color: %1; background: transparent; border: 1px solid %2; "
					"border-radius: 6px; padding: 4px 10px; font-size: 12px; }"
					"QPushButton:hover { background: %3; }")
					.arg(kMuted, kHairline, kHoverTint));
			QObject::connect(copyBtn, &QPushButton::clicked, copyBtn, [copyBtn, sessionKey]() {
				QGuiApplication::clipboard()->setText(sessionKey);
				copyBtn->setText(QStringLiteral("Copied"));
				QTimer::singleShot(1500, copyBtn,
						   [copyBtn]() { copyBtn->setText(QStringLiteral("Copy")); });
			});
			monoLayout->addWidget(copyBtn, 0);

			contentLayout->addWidget(monoFrame);
		}

		auto *footer = new QHBoxLayout();
		footer->addStretch(1);

		// If there is an model download URL we need to add the plumbing to deal with it
		if (!modelUrl.isEmpty()) {
			auto *laterBtn = new QPushButton(QStringLiteral("Not Now"), content);
			laterBtn->setCursor(Qt::PointingHandCursor);
			laterBtn->setStyleSheet(
				QStringLiteral(
					"QPushButton { color: %1; background: transparent; border: 1px solid %2; "
					"border-radius: 8px; padding: 8px 16px; }"
					"QPushButton:hover { background: %3; }")
					.arg(kMuted, kHairline, kHoverTint));
			QObject::connect(laterBtn, &QPushButton::clicked, this, &QDialog::accept);
			footer->addWidget(laterBtn);
		}

		// And we always have an OK butoton
		auto *okBtn = new QPushButton(modelUrl.isEmpty() ? QStringLiteral("OK") : QStringLiteral("Download"),
					      content);
		okBtn->setCursor(Qt::PointingHandCursor);
		okBtn->setDefault(true);
		okBtn->setStyleSheet(
			QStringLiteral("QPushButton { background: %1; color: %2; font-weight: 600; border: none; "
				       "border-radius: 8px; padding: 8px 22px; }"
				       "QPushButton:hover { background: %3; }"
				       "QPushButton:pressed { background: %4; }")
				.arg(accent.name(), kTopText, accent.lighter(115).name(), accent.darker(110).name()));
		QObject::connect(okBtn, &QPushButton::clicked, this, [this, modelUrl]() {
			if (!modelUrl.isEmpty())
				QDesktopServices::openUrl(QUrl(modelUrl));
			accept();
		});
		footer->addWidget(okBtn);

		contentLayout->addLayout(footer);
	}
};

} // end of namespace

void showBabelStreamerPopup(PopupKind kind, const std::string &title, const std::string &body,
			    const std::string &sessionKey, const std::string &modelURL)
{
	struct Args {
		PopupKind kind;
		QString title;
		QString body;
		QString copyValue;
		QString actionUrl;
	};
	auto *args = new Args{kind, QString::fromStdString(title), QString::fromStdString(body),
			      QString::fromStdString(sessionKey), QString::fromStdString(modelURL)};

	/*
       Popups are raised from a detached worker thread (see WhisperFilter.cpp);
       QWidgets may only be run on the Qt UI thread, which is what OBS's
       main thread runs.
     */
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			auto *args = static_cast<Args *>(param);
			auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
			auto *dialog = new BabelStreamerDialogue(args->kind, args->title, args->body, args->copyValue,
								 args->actionUrl, parent);
			delete args;
			dialog->setWindowModality(Qt::NonModal);
			dialog->show();
			dialog->raise();
			dialog->activateWindow();
		},
		args, false);
}
