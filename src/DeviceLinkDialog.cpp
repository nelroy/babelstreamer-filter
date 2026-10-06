// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  DeviceLinkDialog.cpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//=======================================================================

#include "DeviceLinkDialog.hpp"
#include "BrandPalette.hpp"
#include "TlsBackend.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

/* Default URL if no URL passed in */
constexpr const char *kDefaultApiBase = "https://babelstreamer.com/api";
using namespace brandpalette;

/**
    Pimpl to hide main dialogue methods
 */
class Pimpl : public QDialog {
public:
	Pimpl(QWidget *parent, const QString &apiBase) : QDialog(parent)
	{
		/* Get the base URL to access the wss: api - default if not passed in*/
		myApiBase = apiBase.isEmpty() ? QString::fromUtf8(kDefaultApiBase) : apiBase;

		/* set up the net access manager */
		networkAccessManager = new QNetworkAccessManager(this);

		/* Set up the dialogue basics */
		setWindowTitle(QStringLiteral("BabelStreamer"));
		setFixedWidth(460);
		setModal(true);
		setWindowFlags((windowFlags() | Qt::CustomizeWindowHint) & ~Qt::WindowCloseButtonHint);
		setStyleSheet(QStringLiteral("QDialog { background: %1; }").arg(kBckg));

		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(0, 0, 0, 0);
		root->setSpacing(0);

		auto *accentBar = new QFrame(this);
		accentBar->setFixedHeight(4);
		accentBar->setStyleSheet(QStringLiteral("background: %1; border: none;").arg(kAccentTeal));
		root->addWidget(accentBar);

		auto *content = new QWidget(this);
		auto *layout = new QVBoxLayout(content);
		layout->setContentsMargins(28, 24, 28, 24);
		layout->setSpacing(14);
		root->addWidget(content);

		/* Title */
		auto *titleLabel = new QLabel(QStringLiteral("Connect your BabelStreamer account"), content);
		QFont titleFont = titleLabel->font();
		titleFont.setBold(true);
		titleFont.setPointSize(titleFont.pointSize() + 3);
		titleLabel->setFont(titleFont);
		titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(kText));
		titleLabel->setWordWrap(true);
		layout->addWidget(titleLabel);

		/* User instruction text and label*/
		instructionLabel =
			new QLabel(QStringLiteral("Check that the code on the sign-in page matches the one below "
						  "before you approve it. That match is what proves the page is "
						  "linking this copy of OBS, and not someone else's."),
				   content);
		instructionLabel->setWordWrap(true);
		instructionLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 13px;").arg(kText));
		layout->addWidget(instructionLabel);

		codeLabel = new QLabel(QStringLiteral("……"), content);
		codeLabel->setAlignment(Qt::AlignCenter);
		codeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
		{
			QFont f = codeLabel->font();
			f.setStyleHint(QFont::Monospace);
			f.setFamily(QStringLiteral("monospace"));
			f.setPointSize(f.pointSize() + 12);
			f.setBold(true);
			f.setLetterSpacing(QFont::PercentageSpacing, 130);
			codeLabel->setFont(f);
		}
		codeLabel->setStyleSheet(
			QStringLiteral("color: %1; background: %2; border: 1px solid %3; border-radius: 10px; "
				       "padding: 14px 8px;")
				.arg(kAccentTeal, kPanel, kHairline));
		layout->addWidget(codeLabel);

		statusLabel = new QLabel(QStringLiteral("Requesting a code…"), content);
		statusLabel->setWordWrap(true);
		statusLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(kMuted));
		layout->addWidget(statusLabel);

		auto *footer = new QHBoxLayout();
		footer->setSpacing(8);

		/* Control Buttons */
		openBtn = new QPushButton(QStringLiteral("Open sign-in page"), content);
		openBtn->setCursor(Qt::PointingHandCursor);
		openBtn->setEnabled(false);
		openBtn->setStyleSheet(
			QStringLiteral("QPushButton { color: %1; background: %2; border: none; border-radius: 6px; "
				       "padding: 8px 16px; font-weight: bold; }"
				       "QPushButton:hover { background: #7defd9; }"
				       "QPushButton:disabled { color: %3; background: %4; }")
				.arg(kTopText, kAccentTeal, kMuted, kPanel));
		connect(openBtn, &QPushButton::clicked, this, [this]() {
			if (!verifyUriComplete.isEmpty())
				QDesktopServices::openUrl(QUrl(verifyUriComplete));
		});
		footer->addWidget(openBtn);
		footer->addStretch(1);

		closeBtn = new QPushButton(QStringLiteral("Cancel"), content);
		closeBtn->setCursor(Qt::PointingHandCursor);
		closeBtn->setStyleSheet(ghostBtnStyle());
		connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
		footer->addWidget(closeBtn);

		layout->addLayout(footer);

		/* Now we start polling for a reply. Won't actually do anythig until polling is set up later */
		pollTimer = new QTimer(this);
		connect(pollTimer, &QTimer::timeout, this, [this]() { pollConnectionCompleted(); });
	}

	/* This is the function to actually start the dialogue with the server. Should be called after the
        popup is created
     */
	void beginServerDialogue()
	{
		/* Contact the server with the request and a macine id. If all OK then we should get a
           device code from the server
         */
		QJsonObject requestBody;
		requestBody.insert(QStringLiteral("label"), QSysInfo::machineHostName());
		QNetworkReply *reply = post(QStringLiteral("/device/code"), requestBody);
		connect(reply, &QNetworkReply::finished, this, [this, reply]() {
			reply->deleteLater(); // schedule for deletion when return to event loop

			if (reply->error() != QNetworkReply::NoError) {
				fail(QStringLiteral("Couldn't reach BabelStreamer to start sign-in:\n%1")
					     .arg(reply->errorString()));
				return;
			}

			/* Get the data from the reply - device code and user code */
			const QJsonObject serverData = QJsonDocument::fromJson(reply->readAll()).object();
			deviceCode = serverData.value(QStringLiteral("deviceCode")).toString();
			userCodeDisplay = serverData.value(QStringLiteral("userCode")).toString();

			verifyUriComplete = serverData.value(QStringLiteral("verificationUriComplete")).toString();
			intervalSec = serverData.value(QStringLiteral("interval")).toInt(5);
			const int expiresIn = serverData.value(QStringLiteral("expiresIn")).toInt(900);

			if (deviceCode.isEmpty() || userCodeDisplay.isEmpty()) {
				fail(QStringLiteral("BabelStreamer returned an unexpected response. "
						    "Please try again."));
				return;
			}

			/* Now we start waiting for the user to complete linking on the server/website.*/
			expiryEpoch = QDateTime::currentSecsSinceEpoch() + expiresIn;
			codeLabel->setText(userCodeDisplay);
			statusLabel->setText(QStringLiteral("Waiting for you to approve this code on the website…"));
			openBtn->setEnabled(!verifyUriComplete.isEmpty());

			if (intervalSec < 1)
				intervalSec = 5;
			pollTimer->start(intervalSec * 1000);
		});
	}

	/* Getters for results - undefined if not completed */
	bool succeeded() const { return dialogueSucceeded; }
	std::string token() const { return returnToken.toStdString(); }
	std::string label() const { return returnLabel.toStdString(); }

private:
	static QString ghostBtnStyle()
	{
		return QStringLiteral("QPushButton { color: %1; background: transparent; border: 1px solid %2; "
				      "border-radius: 6px; padding: 8px 16px; }"
				      "QPushButton:hover { background: %3; }"
				      "QPushButton:disabled { color: %4; }")
			.arg(kMuted, kHairline, kHoverTint, kDisabled);
	}

	QNetworkReply *post(const QString &pathSuffix, const QJsonObject &body)
	{
		QNetworkRequest req{QUrl(myApiBase + pathSuffix)};
		req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
		req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
		return networkAccessManager->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
	}

	/* This is the poolling routine that is continuously called until
        1) There is a result
        2) There is an error (some errors will not lead to an immediate stop)
        3) There is a timeout
     
        "polling" is used to flag the case where the routine is called before the previous instance
        has coompleted. The routine POSTs a request to the server and handles the reply approproiately
     */
	void pollConnectionCompleted()
	{
		if (polling)
			return; // don't stack a second request if one's still in flight

		/* Handle exhaustion of code expiry*/
		if (QDateTime::currentSecsSinceEpoch() >= expiryEpoch) {
			fail(QStringLiteral("This code expired before it was approved. Close this and click "
					    "\"Connect account\" again for a new one."));
			return;
		}

		/* Now handle the request/reply */
		polling = true;
		QJsonObject body;
		body.insert(QStringLiteral("deviceCode"), deviceCode);
		QNetworkReply *reply = post(QStringLiteral("/device/token"), body);

		connect(reply, &QNetworkReply::finished, this, [this, reply]() {
			reply->deleteLater(); // will delete after return to measage queu
			polling = false;

			const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

			// Status 429 == too many requests - back the poll interval off and keep waiting.
			if (httpStatus == 429) {
				intervalSec += 5;
				pollTimer->start(intervalSec * 1000);
				return;
			}

			/* retry on network error -  keep polling, just note it.*/
			if (reply->error() != QNetworkReply::NoError && httpStatus == 0) {
				statusLabel->setText(QStringLiteral("Network hiccup - still waiting for approval…"));
				return;
			}

			/* We have a reply - parse and process*/
			const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
			const QString status = obj.value(QStringLiteral("status")).toString();

			/* Request approved (good case), checj we have a token (otherwise fail) and return */
			if (status == QStringLiteral("approved")) {
				returnToken = obj.value(QStringLiteral("token")).toString();
				returnLabel = obj.value(QStringLiteral("label")).toString();
				if (returnToken.isEmpty()) {
					fail(QStringLiteral("Approved, but no token came back. Please try again."));
					return;
				}
				pollTimer->stop();
				dialogueSucceeded = true;
				accept();
				return;
			}

			/* request may be denied - tell the streamer */
			if (status == QStringLiteral("denied")) {
				fail(QStringLiteral("This request was declined on the website."));
				return;
			}

			/* request code may have expired (according to server) - tell the streamer*/
			if (status == QStringLiteral("expired") || status == QStringLiteral("not_found")) {
				fail(QStringLiteral("This code is no longer valid. Close this and click "
						    "\"Connect account\" again for a new one."));
				return;
			}

			/* status == "pending" (or anything unexpected): keep waiting. */
			statusLabel->setText(QStringLiteral("Waiting for you to approve this code on the website…"));
		});
	}

	/* Handle failure consistently by posting the failure message and shutting down the poll*/
	void fail(const QString &msg)
	{
		pollTimer->stop();
		codeLabel->setStyleSheet(
			QStringLiteral("color: %1; background: %2; border: 1px solid %3; border-radius: 10px; "
				       "padding: 14px 8px;")
				.arg(kMuted, kPanel, kHairline));
		statusLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(kAccentCoral));
		statusLabel->setText(msg);
		openBtn->setEnabled(false);
		closeBtn->setText(QStringLiteral("Close"));
	}

	/* Internal housekeeping */
	QNetworkAccessManager *networkAccessManager = nullptr;
	QTimer *pollTimer = nullptr;

	QLabel *instructionLabel = nullptr;
	QLabel *codeLabel = nullptr;
	QLabel *statusLabel = nullptr;
	QPushButton *openBtn = nullptr;
	QPushButton *closeBtn = nullptr;

	QString myApiBase;
	QString deviceCode;
	QString userCodeDisplay;
	QString verifyUriComplete;
	int intervalSec = 5;
	qint64 expiryEpoch = 0;
	bool polling = false; // Note: we are assuming that bool is atomic across all architectures

	QString returnToken;
	QString returnLabel;
	bool dialogueSucceeded = false;
};

/* Entry point set up and start the Pimpl */
void connectAccountViaDeviceCode(const std::string &apiBase,
				 std::function<void(const std::string &, const std::string &)> onComplete)
{
	ensureTlsBackendAvailable();

	const QString base = QString::fromStdString(apiBase);
	blog(LOG_INFO, "[babelstreamer-filter] Device-code pairing via %s",
	     base.isEmpty() ? kDefaultApiBase : base.toUtf8().constData());

	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *dialog = new Pimpl(parent, base);
	dialog->beginServerDialogue(); // Start the communication with the server
	dialog->exec();                // show dialogue as modal dialogue. Will return when dialogue is complete

	const bool ok = dialog->succeeded();
	const std::string token = ok ? dialog->token() : std::string();
	const std::string label = ok ? dialog->label() : std::string();
	dialog->deleteLater();

	if (ok) {
		blog(LOG_INFO, "[babelstreamer-filter] Device-code pairing succeeded%s",
		     label.empty() ? "" : (" for \"" + label + "\"").c_str());
	} else {
		blog(LOG_INFO, "[babelstreamer-filter] Device-code pairing cancelled or failed");
	}

	/* Report to caller asynchronously */
	onComplete(token, label);
}

/* This is an extra dialogue to ask the user if they want to start the linking process*/
bool askToConnectAccount()
{
	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	const auto answer = QMessageBox::question(
		parent, QStringLiteral("BabelStreamer"),
		QStringLiteral("BabelStreamer is not connected to your account yet, so it has nowhere "
			       "to send captions.\n\nConnect it now?\n\nYou can always do this later "
			       "with the \"Connect account\" button in the filter's settings."),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
	return answer == QMessageBox::Yes;
}
