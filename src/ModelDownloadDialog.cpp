// SPDX-FileCopyrightText: 2026 Kamayu B.V. <https://www.kamayu.nl>
// SPDX-License-Identifier: GPL-2.0-or-later

//=======================================================================
//
//  ModelDownloadDialog.cpp
//
//  Created: 20 sep 2026
//
//  Author: N.H. Elroy with support from Claude Code (Sonnet)
//
//=======================================================================

#include "ModelDownloadDialog.hpp"
#include "BrandPalette.hpp"
#include "TlsBackend.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <mbedtls/sha256.h>

#include <iomanip>
#include <sstream>
#include <thread>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QRadioButton>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace {
/*
    The standard Whisper models are defined here for English only and multilingual. The
    sha256 hashes for these models are also defined, which implies that the download is
    pinned to a specific commit, otherwise this doesn't work. This is only ofor the standard
    models.
 */

struct StandardModel {
	const char *filename;
	const char *url;
	const char *sha256;
	qint64 bytes; // for the size shown in the chooser
};

constexpr StandardModel kEnglishModel = {
	"ggml-medium.en.bin",
	"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.en.bin",
	"cc37e93478338ec7700281a7ac30a10128929eb8f427dda2e865faa8f6da4356",
	1533774781,
};

constexpr StandardModel kMultilingualModel = {
	"ggml-large-v3-q5_0.bin",
	"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-q5_0.bin",
	"d75795ecff3f83b5faa89d1900604ad8c780abd5739fae406de19f23ecd98ad1",
	1081140203,
};

// "1.5 GB" - for telling the streamer what they are about to download.
QString humanSize(qint64 bytes)
{
	return QStringLiteral("%1 GB").arg(bytes / 1e9, 0, 'f', 1);
}

/*
    Do a filetreaming check on the SHA256 checksum
 */
std::string fileSha256Hex(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return {};

	mbedtls_sha256_context ctx;
	mbedtls_sha256_init(&ctx);
	mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256, not the SHA-224 variant

	QByteArray chunk;
	while (!(chunk = f.read(1 << 20)).isEmpty()) {
		mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char *>(chunk.constData()),
				      static_cast<size_t>(chunk.size()));
	}

	unsigned char digest[32];
	mbedtls_sha256_finish(&ctx, digest);
	mbedtls_sha256_free(&ctx);

	std::ostringstream oss;
	oss << std::hex << std::setfill('0');
	for (unsigned char b : digest)
		oss << std::setw(2) << int(b);
	return oss.str();
}

using namespace brandpalette;

/*
    Download progress dialogue. Put up once downloading starts and shows a progress bar
 */
class DownloadDialog : public QDialog {
public:
	/*
        Main dialogue. Displays a progress bar and explanatory text. Styled ot BabelStreamer styles
     */
	explicit DownloadDialog(QWidget *parent, const QString &titleText = QStringLiteral("Downloading speech model"))
		: QDialog(parent)
	{
		setWindowTitle(QStringLiteral("BabelStreamer"));
		setFixedWidth(440);
		setModal(true);

		// cancelling only via the explicit Cancel button , so we always know why the dialog closed.
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

		auto *titleLabel = new QLabel(titleText, content);
		QFont titleFont = titleLabel->font();
		titleFont.setBold(true);
		titleFont.setPointSize(titleFont.pointSize() + 3);
		titleLabel->setFont(titleFont);
		titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(kText));
		layout->addWidget(titleLabel);

		statusLabel = new QLabel(QStringLiteral("Starting…"), content);
		statusLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 12px;").arg(kMuted));
		layout->addWidget(statusLabel);

		progressBar = new QProgressBar(content);
		progressBar->setRange(0, 0); // indeterminate until the first progress signal
		progressBar->setTextVisible(false);
		progressBar->setFixedHeight(20);
		progressBar->setStyleSheet(
			QStringLiteral("QProgressBar { background: %1; border: 1px solid %2; border-radius: 8px; }"
				       "QProgressBar::chunk { background: %3; border-radius: 8px; }")
				.arg(kPanel, kHairline, kAccentTeal));
		layout->addWidget(progressBar);

		auto *footer = new QHBoxLayout();
		footer->addStretch(1);
		cancelBtn = new QPushButton(QStringLiteral("Cancel"), content);
		cancelBtn->setCursor(Qt::PointingHandCursor);
		cancelBtn->setStyleSheet(
			QStringLiteral("QPushButton { color: %1; background: transparent; border: 1px solid %2; "
				       "border-radius: 6px; padding: 6px 16px; }"
				       "QPushButton:hover { background: %3; }")
				.arg(kMuted, kHairline, kHoverTint));
		connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
		footer->addWidget(cancelBtn);
		layout->addLayout(footer);
	}

	/*
        update the prgress bar position
     */
	void setProgress(qint64 received, qint64 total)
	{
		const double receivedMb = received / 1000000.0;
		if (total > 0) {
			progressBar->setRange(0, 100);
			progressBar->setValue(int(received * 100 / total));
			statusLabel->setText(QStringLiteral("%1 MB / %2 MB")
						     .arg(receivedMb, 0, 'f', 1)
						     .arg(total / 1000000.0, 0, 'f', 1));
		} else {
			progressBar->setRange(0, 0); // server didn't send a length - spin indeterminately
			statusLabel->setText(QStringLiteral("%1 MB downloaded").arg(receivedMb, 0, 'f', 1));
		}
	}

	// Set from the network reply's `finished` handler only - stays false on
	// the cancel path, since that calls reject() directly without going
	// through here.
	void markSucceeded(bool ok) { success = ok; }
	bool succeeded() const { return success; }

	void setStatusText(const QString &t) { statusLabel->setText(t); }

	QPushButton *cancelBtn;

private:
	QLabel *statusLabel;
	QProgressBar *progressBar;
	bool success = false;
};

/*
    Download the actual Whisper model file and verify it against the SHA256 checksum
    Shows appropriate dialogues where necessary
 */
void downloadModelFile(const StandardModel &model, std::function<void(const std::string &)> onComplete)
{
	ensureTlsBackendAvailable();

	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());

	// Get the config path
	QString startDir;
	if (char *cfgPath = obs_module_config_path(nullptr)) {
		startDir = QString::fromUtf8(cfgPath);
		bfree(cfgPath);
	}
	if (startDir.isEmpty() || !QDir(startDir).exists())
		startDir = QDir::homePath();

	/*
        An extra dialogue to tell the user that a directory-picker is coming. This removes
        ambiguity and also I can't find a way to let Qt display the caption in the picker
        on MacOS
     */
	QMessageBox::information(parent, QStringLiteral("BabelStreamer"),
				 QStringLiteral("Choose a folder to store the model."));

	const QString dir = QFileDialog::getExistingDirectory(
		parent, QStringLiteral("Choose a folder to store the model"), startDir);
	if (dir.isEmpty()) {
		onComplete(std::string());
		return;
	}

	// Note the ".part" - we're saving to a temp file which will be validated before comitting
	const QString finalPath = QDir(dir).filePath(QString::fromUtf8(model.filename));
	const QString partPath = finalPath + QStringLiteral(".part");

	// File already there? Does do the hash check to verify that the file is correct and then
	// we can call it a day
	{
		const QFileInfo have(finalPath);
		if (have.isFile() && have.size() == model.bytes && fileSha256Hex(finalPath) == model.sha256) {
			blog(LOG_INFO,
			     "[babelstreamer-filter] '%s' is already present and verified - "
			     "using it instead of downloading it again",
			     model.filename);
			onComplete(finalPath.toStdString());
			return;
		}
	}

	// Now let's try and create a file to store the download
	auto *file = new QFile(partPath);
	if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		blog(LOG_WARNING, "[babelstreamer-filter] Could not open '%s' for writing the downloaded model",
		     partPath.toUtf8().constData());
		delete file;
		QMessageBox::warning(parent, QStringLiteral("BabelStreamer"),
				     QStringLiteral("Could not write to that folder:\n%1").arg(dir));
		onComplete(std::string());
		return;
	}

	/*
        This is where the meat starts. Create the dialogue and start the download process. This is all
        asynchronous and by the following event handling:
	
        - downloadProgress: report progress
        - readyRead: write another block (file to big to buffer in memory)
        - finished: drain and clean up
	
        Also the dialogue's Cancel button is captured, which will signal abort to Qt and this will
        cascade to the finished event.
     */

	auto *dialog = new DownloadDialog(parent);
	auto *nam = new QNetworkAccessManager(dialog);

	QNetworkRequest req(QUrl(QString::fromUtf8(model.url)));
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	QNetworkReply *reply = nam->get(req);

	QObject::connect(reply, &QNetworkReply::downloadProgress, dialog,
			 [dialog](qint64 received, qint64 total) { dialog->setProgress(received, total); });

	QObject::connect(reply, &QNetworkReply::readyRead, dialog, [reply, file]() { file->write(reply->readAll()); });

	QObject::connect(dialog->cancelBtn, &QPushButton::clicked, reply, &QNetworkReply::abort);

	// Bound to `dialog` as receiver
	// automatically rather than calling into a dangling dialog.
	QObject::connect(reply, &QNetworkReply::finished, dialog, [dialog, reply, file]() {
		file->write(reply->readAll());
		file->close();
		dialog->markSucceeded(reply->error() == QNetworkReply::NoError);
		dialog->accept();
	});

	dialog->exec(); // blocks this thread's nested Qt loop until accept()/reject()

	// Now we work out what happened and handle the error modes.
	const bool succeeded = dialog->succeeded();
	const QNetworkReply::NetworkError err = reply->error();
	const QString errString = reply->errorString();
	reply->deleteLater();

	if (file->isOpen())
		file->close();

	if (!succeeded) {
		file->remove();
		delete file;
		dialog->deleteLater();
		if (err != QNetworkReply::OperationCanceledError) {
			blog(LOG_WARNING, "[babelstreamer-filter] Model download failed: %s",
			     errString.toUtf8().constData());
			QMessageBox::warning(parent, QStringLiteral("BabelStreamer"),
					     QStringLiteral("Downloading the model failed:\n%1").arg(errString));
		}
		onComplete(std::string());
		return;
	}

	delete file;
	dialog->deleteLater();

	// Before finalsing we do the hash check
	const std::string actualHash = fileSha256Hex(partPath);
	if (actualHash != model.sha256) {
		blog(LOG_WARNING,
		     "[babelstreamer-filter] Downloaded model failed integrity check "
		     "(expected %s, got %s) - refusing to use it",
		     model.sha256, actualHash.empty() ? "<unreadable>" : actualHash.c_str());
		QFile::remove(partPath);
		QMessageBox::warning(parent, QStringLiteral("BabelStreamer"),
				     QStringLiteral("The downloaded model failed an integrity check and was "
						    "discarded. This can mean a corrupted download - please try "
						    "again - or that the file served at the download URL no "
						    "longer matches what BabelStreamer expects."));
		onComplete(std::string());
		return;
	}

	// Everything OK - rename and signal successs
	QFile::remove(finalPath); // clear out any stale previous copy before the rename
	if (!QFile::rename(partPath, finalPath)) {
		blog(LOG_WARNING, "[babelstreamer-filter] Downloaded model but could not rename '%s' to '%s'",
		     partPath.toUtf8().constData(), finalPath.toUtf8().constData());
		QMessageBox::warning(parent, QStringLiteral("BabelStreamer"),
				     QStringLiteral("Downloaded the model but could not finalize it in:\n%1").arg(dir));
		onComplete(std::string());
		return;
	}

	onComplete(finalPath.toStdString());
}

} // namespace

/*
    For Mac Neural Engine we also need a prebuilr Core ML decoder. These are available
    for most Whisper models on the same Hugging Face repo. The user can choose a standard
    model or download their own model. If they download their own model the the download
    process using naming conventions to look for the matching Core ML decoder. The standard
    models have a standard core ML decoder. As with the whisper models we hard code a SHA256
    checksum which means we have to pin the decoder to a specific commit (the same as for
    the whisper model). Decoders for any other model are still fetchable, they just don't
    have this extra level of security
 */
namespace // for the Core ML models
{

struct PinnedEncoder {
	const char *dirName;
	const char *url;
	const char *sha256;
	qint64 bytes;
};

// Standard mlmodelc are pinned at the 2026-09-19 version, with matching SHA256 checksum
constexpr PinnedEncoder kPinnedEncoders[] = {
	{
		"ggml-large-v3-encoder.mlmodelc",
		"https://huggingface.co/ggerganov/whisper.cpp/resolve/"
		"5359861c739e955e79d9a303bcbc70fb988958b1/ggml-large-v3-encoder.mlmodelc.zip",
		"47837be7594a29429ec08620043390c4d6d467f8bd362df09e9390ace76a55a4",
		1175711232,
	},
	{
		"ggml-medium.en-encoder.mlmodelc",
		"https://huggingface.co/ggerganov/whisper.cpp/resolve/"
		"5359861c739e955e79d9a303bcbc70fb988958b1/ggml-medium.en-encoder.mlmodelc.zip",
		"cdc44fee3c62b5743913e3147ed75f4e8ecfb52dd7a0f0f7387094b406ff0ee6",
		566993085,
	},
};

// Check if there is a pinned encoder already available
const PinnedEncoder *findPinnedEncoder(const std::string &dirName)
{
	for (const auto &e : kPinnedEncoders) {
		if (dirName == e.dirName)
			return &e;
	}
	return nullptr;
}

// Check if Apple silicon, which implies availability of Core ML
#if defined(__APPLE__)
bool macIsAppleSilicon()
{
	int val = 0;
	size_t sz = sizeof(val);
	if (sysctlbyname("hw.optional.arm64", &val, &sz, nullptr, 0) != 0)
		return false;
	return val != 0;
}
#endif

#if HAS_WHISPER_COREML
// Standalone Core ML loader from libwhisper.coreml.a (C linkage; declared in
// whisper.cpp's private coreml/whisper-encoder.h, not the installed headers).
extern "C" {
struct whisper_coreml_context;
struct whisper_coreml_context *whisper_coreml_init(const char *path_model);
void whisper_coreml_free(struct whisper_coreml_context *ctx);
}

/*
    The Core ML model needs to be compiled before first usage. Do this on a worker thread
    to avoid freezing everything up. The dialogue shows a "working" bar
 */
void runCoremlWarmup(QWidget *parent, const QString &mlmodelcPath)
{
	DownloadDialog dlg(parent, QStringLiteral("Preparing Neural Engine"));
	dlg.cancelBtn->setEnabled(false); // not cancellable - a partial compile is useless
	dlg.setStatusText(QStringLiteral("Optimizing for your Neural Engine - one-time, up to a minute…"));

	const std::string path = mlmodelcPath.toStdString();
	std::thread worker([&dlg, path]() {
		if (auto *c = whisper_coreml_init(path.c_str())) {
			whisper_coreml_free(c);
			blog(LOG_INFO, "[babelstreamer-filter] Core ML warm-up complete for %s", path.c_str());
		} else {
			blog(LOG_WARNING,
			     "[babelstreamer-filter] Core ML warm-up could not load %s "
			     "(it will still compile lazily on first use)",
			     path.c_str());
		}
		QMetaObject::invokeMethod(&dlg, "accept", Qt::QueuedConnection);
	});
	dlg.exec();
	worker.join();
}
#endif // HAS_WHISPER_COREML

} // namespace

void runOnUiThreadLater(std::function<void()> fn)
{
	// qApp lives in the UI thread, so a QueuedConnection posts this to that
	// thread's event queue and returns immediately.
	QMetaObject::invokeMethod(qApp, std::move(fn), Qt::QueuedConnection);
}

bool coremlEncoderCached(const std::string &modelPath)
{
	QString cacheDir;
	if (char *c = obs_module_config_path("coreml")) {
		cacheDir = QString::fromUtf8(c);
		bfree(c);
	}
	if (cacheDir.isEmpty())
		return false;
	const QString dir = QDir(cacheDir).filePath(QString::fromUtf8(coremlEncoderNameFor(modelPath).c_str()));
	return QFileInfo::exists(dir);
}

/*
    Download the actual Core ML encoder file. This should never be executed
    if not supported, but there is a guard just in case.
 
    Note that there is an async return via the inComplete which uses a
    CoremlEncoderResult to pass the status.
 */
void downloadCoremlEncoderFor(const std::string &modelPath, bool askFirst,
			      std::function<void(CoremlEncoderResult)> onComplete)
{
#if !defined(__APPLE__)
	onComplete(CoremlEncoderResult::NotAvailable);
	return;
#else
	// The Neural Engine only exists on Apple Silicon, so fail if not
	if (!macIsAppleSilicon()) {
		onComplete(CoremlEncoderResult::NotAvailable);
		return;
	}

	// Get the name of the core ML model and the dir where we can find it
	const std::string dirNameStd = coremlEncoderNameFor(modelPath);
	const QString dirName = QString::fromUtf8(dirNameStd.c_str());

	// If we already have it, then we're done
	if (coremlEncoderCached(modelPath)) {
		onComplete(CoremlEncoderResult::Installed);
		return;
	}

	// We only have a file pinned to a specific commit and with a hascode for the
	// standard models - any other models are resolved without this extra security
	const PinnedEncoder *pinned = findPinnedEncoder(dirNameStd);

	// if we need to ask first put out a jargon-free message box to confirm this is
	// what the user wants.
	if (askFirst) {
		const QString modelName = QFileInfo(QString::fromUtf8(modelPath.c_str())).fileName();
		auto *askParent = static_cast<QWidget *>(obs_frontend_get_main_window());
		const auto answer = QMessageBox::question(
			askParent, QStringLiteral("BabelStreamer"),
			QStringLiteral("Apple Neural Engine support for %1 needs a one-time download.\n\n"
				       "Without it this model runs on the CPU, which may not keep up with "
				       "live speech, or the GPU which may cause stuttering video"
				       "\n\nAll BabelStreamer filters will reload.\n\n"
				       "Download it now?")
				.arg(modelName),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

		if (answer != QMessageBox::Yes) {
			onComplete(CoremlEncoderResult::Cancelled);
			return;
		}
	}

	// Now we get to the point where we actually prepare to download the encoder model
	const QString encoderUrl =
		pinned ? QString::fromUtf8(pinned->url)
		       : QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/%1.zip").arg(dirName);

	ensureTlsBackendAvailable(); // see the doc to see why this is needed
	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());

	// Always goes to the plugin's own coreml cache (which we may need to create) - this is not an object
	// that the user is meant to control
	QString cacheDir;
	if (char *c = obs_module_config_path("coreml")) {
		cacheDir = QString::fromUtf8(c);
		bfree(c);
	}

	if (cacheDir.isEmpty() || !QDir().mkpath(cacheDir)) {
		blog(LOG_WARNING, "[babelstreamer-filter] Could not create the Core ML cache directory");
		onComplete(CoremlEncoderResult::Failed);
		return;
	}

	// Download to intermediary file so that failure doesn't leave a partial or corrupt file
	// masquerading as the real file
	const QString zipPart = QDir(cacheDir).filePath(QStringLiteral("%1.zip.part").arg(dirName));

	auto *file = new QFile(zipPart);
	if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		blog(LOG_WARNING, "[babelstreamer-filter] Could not open '%s' for writing the Core ML encoder",
		     zipPart.toUtf8().constData());
		delete file;
		onComplete(CoremlEncoderResult::Failed);
		return;
	}

	/* Put up the progress dialogue and start downloading. The asynchronous Qt events are handled as:
	
	    - donwoadProgress: trigger progress bar update in dailogue
	    - readyRead: write another block to the temp file
	    - finished: flush and close dialogue
	  
	   Additionally the cancel button from the dialogue triggers a network abort, which in turn will
	   trigger a Qt "finished" event which will close the dialogue. In this way when the dialogue
	   is run modally the download will have been completed or aborted when the exec() returns
	*/
	auto *dialogue = new DownloadDialog(parent, QStringLiteral("Downloading Neural Engine model"));
	auto *nam = new QNetworkAccessManager(dialogue);

	QNetworkRequest req{QUrl(encoderUrl)};
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	QNetworkReply *reply = nam->get(req);

	QObject::connect(reply, &QNetworkReply::downloadProgress, dialogue,
			 [dialogue](qint64 received, qint64 total) { dialogue->setProgress(received, total); });
	QObject::connect(reply, &QNetworkReply::readyRead, dialogue,
			 [reply, file]() { file->write(reply->readAll()); });
	QObject::connect(dialogue->cancelBtn, &QPushButton::clicked, reply, &QNetworkReply::abort);

	// When the download is done then save the file and tidy up
	QObject::connect(reply, &QNetworkReply::finished, dialogue, [dialogue, reply, file]() {
		file->write(reply->readAll());
		file->close();
		dialogue->markSucceeded(reply->error() == QNetworkReply::NoError);
		dialogue->accept();
	});
	dialogue->exec();

	/*
        Now we look at what happened and react accordingly. Failure modes are
     cancelled, file not found, or some other failure which will be logged
	*/
	const bool succeeded = dialogue->succeeded();
	const QNetworkReply::NetworkError err = reply->error();
	const QString errString = reply->errorString();
	reply->deleteLater();
	if (file->isOpen())
		file->close();
	dialogue->deleteLater();

	// Error cases - report and log
	if (!succeeded) {
		file->remove();
		delete file;

		if (err == QNetworkReply::OperationCanceledError) {
			onComplete(CoremlEncoderResult::Cancelled);
			return;
		}
		if (err == QNetworkReply::ContentNotFoundError) {
			// No published encoder for this model
			blog(LOG_INFO, "[babelstreamer-filter] No Neural Engine encoder is published for %s",
			     dirName.toUtf8().constData());
			onComplete(CoremlEncoderResult::NotAvailable);
			return;
		}
		if (err != QNetworkReply::OperationCanceledError) {
			blog(LOG_WARNING, "[babelstreamer-filter] Core ML encoder download failed: %s",
			     errString.toUtf8().constData());
			QMessageBox::warning(
				parent, QStringLiteral("BabelStreamer"),
				QStringLiteral("Downloading the Neural Engine model failed:\n%1").arg(errString));
		}
		onComplete(CoremlEncoderResult::Failed);
		return;
	}
	delete file;

	// If we're using a pinned standard model verify the hash
	const std::string actualHash = pinned ? fileSha256Hex(zipPart) : std::string();
	if (pinned && actualHash != pinned->sha256) {
		blog(LOG_WARNING,
		     "[babelstreamer-filter] Core ML encoder failed integrity check (expected %s, got %s) - discarding",
		     pinned->sha256, actualHash.empty() ? "<unreadable>" : actualHash.c_str());
		QFile::remove(zipPart);
		QMessageBox::warning(parent, QStringLiteral("BabelStreamer"),
				     QStringLiteral("The downloaded Neural Engine model failed an integrity check "
						    "and was discarded. Please try again."));
		onComplete(CoremlEncoderResult::Failed);
		return;
	}

	// Unzip into a scratch dir, then atomically swap the .mlmodelc into place.
	const QString extractDir = QDir(cacheDir).filePath(QStringLiteral(".extract"));
	QDir(extractDir).removeRecursively();
	QDir().mkpath(extractDir);

	QProcess unzipper;
	unzipper.start(QStringLiteral("/usr/bin/ditto"),
		       {QStringLiteral("-x"), QStringLiteral("-k"), zipPart, extractDir});
	unzipper.waitForFinished(-1);
	const bool extractOk = (unzipper.exitStatus() == QProcess::NormalExit && unzipper.exitCode() == 0);

	const QString extracted = QDir(extractDir).filePath(dirName);
	if (!extractOk || !QFileInfo::exists(extracted)) {
		blog(LOG_WARNING, "[babelstreamer-filter] Could not extract the Core ML encoder (ditto exit %d)",
		     unzipper.exitCode());
		QDir(extractDir).removeRecursively();
		QFile::remove(zipPart);
		QMessageBox::warning(parent, QStringLiteral("BabelStreamer"),
				     QStringLiteral("Could not unpack the Neural Engine model. Please try again."));
		onComplete(CoremlEncoderResult::Failed);
		return;
	}

	const QString finalDir = QDir(cacheDir).filePath(dirName);
	QDir(finalDir).removeRecursively(); // clear any stale previous copy
	const bool moved = QDir().rename(extracted, finalDir);
	QDir(extractDir).removeRecursively();
	QFile::remove(zipPart);

	// Error handling for the final move
	if (!moved) {
		blog(LOG_WARNING, "[babelstreamer-filter] Could not move the Core ML encoder into place at %s",
		     finalDir.toUtf8().constData());
		onComplete(CoremlEncoderResult::Failed);
		return;
	}

	blog(LOG_INFO, "[babelstreamer-filter] Core ML encoder installed at %s", finalDir.toUtf8().constData());

#if HAS_WHISPER_COREML
	// Compile it for the Core ML now (behind its own modal), so the first real use
	// is fast and doesn't freeze OBS.
	runCoremlWarmup(parent, finalDir);
#endif

	onComplete(CoremlEncoderResult::Installed);
#endif // __APPLE__

	// congratulations - you finally got to the end of the function
}

// "You need a model"  download. In a namespace to make it invisible outside this module
namespace {

/*
    Structure to pass results of chooser back
 */
struct ChooserResult {
	bool accepted = false;
	bool englishOnly = true;
	std::string deviceId;
};

/*
    Get the size of download the model, plus possiblye the Core ML encoder
    if on Mac and ML core is chosen (Neural EnginO)
*/
qint64 totalDownloadBytes(const StandardModel &model, bool wantCoreML)
{
	qint64 total = model.bytes;
	// Skip an encoder already in the cache
	if (wantCoreML && !coremlEncoderCached(model.filename)) {
		if (const PinnedEncoder *e = findPinnedEncoder(coremlEncoderNameFor(model.filename)))
			total += e->bytes;
	}
	return total;
}

/*
    This is the dialogue that asks for the type of model needed and the processor type.
    - model language (English only or multilingual)
    - host device (list provided in devices: GPU, CPU and on appropriate Apple devices Neural Engine)
 */
ChooserResult askForModelChoice(QWidget *parent, const std::vector<ProcessingDeviceOption> &devices,
				const std::string &currentDevice, const std::string &neuralDeviceId)
{
	ChooserResult chooserResult;

	QDialog dlg(parent);
	dlg.setWindowTitle(QStringLiteral("BabelStreamer - Download Model"));
	auto *root = new QVBoxLayout(&dlg);

	auto *intro = new QLabel(QStringLiteral("Choose what you need. This is a one-time download."));
	intro->setWordWrap(true);
	root->addWidget(intro);

	// Set up checkmarks and hint texts to choose the language type
	auto *langBox = new QGroupBox(QStringLiteral("Languages"), &dlg);
	auto *langLayout = new QVBoxLayout(langBox);
	auto *englishBtn = new QRadioButton(QStringLiteral("English only"), langBox);
	auto *englishHint = new QLabel(
		QStringLiteral("Smaller, and more accurate on English than the multilingual model."), langBox);
	auto *allBtn = new QRadioButton(QStringLiteral("All languages"), langBox);
	auto *allHint = new QLabel(QStringLiteral("Needed to caption anything other than English."), langBox);

	for (QLabel *hint : {englishHint, allHint}) {
		hint->setWordWrap(true);
		hint->setEnabled(false); // renders as secondary text
		hint->setContentsMargins(20, 0, 0, 6);
	}

	englishBtn->setChecked(true);
	langLayout->addWidget(englishBtn);
	langLayout->addWidget(englishHint);
	langLayout->addWidget(allBtn);
	langLayout->addWidget(allHint);
	root->addWidget(langBox);

	// setup dropdown to choose a processing device for the model - on Mac
	// this affects whether we need a core ML encoder
	auto *deviceBox = new QGroupBox(QStringLiteral("Processing device"), &dlg);
	auto *deviceLayout = new QVBoxLayout(deviceBox);
	auto *deviceComboBox = new QComboBox(deviceBox);

	for (const auto &d : devices) {
		deviceComboBox->addItem(QString::fromUtf8(d.label.c_str()), QString::fromUtf8(d.id.c_str()));
	}

	auto *devHint = new QLabel(deviceBox);
	devHint->setWordWrap(true);
	deviceLayout->addWidget(deviceComboBox);
	deviceLayout->addWidget(devHint);
	root->addWidget(deviceBox);

	// Preselect: what the filter already uses, else the Neural Engine, else the
	// first entry that is not the CPU fallback.
	auto indexOfId = [deviceComboBox](const std::string &id) {
		return id.empty() ? -1 : deviceComboBox->findData(QString::fromUtf8(id.c_str()));
	};

	int initial = indexOfId(currentDevice);
	if (initial < 0) {
		initial = indexOfId(neuralDeviceId);
	}
	if (initial < 0) {
		for (int i = 0; i < deviceComboBox->count(); ++i) {
			if (deviceComboBox->itemData(i).toString() != QStringLiteral("cpu")) {
				initial = i;
				break;
			}
		}
	}
	deviceComboBox->setCurrentIndex(initial < 0 ? 0 : initial);

	auto *downloadSizeLabel = new QLabel(&dlg);
	root->addWidget(downloadSizeLabel);

	// lambda to refresh the dialogue. Note the parameters are capture by value
	// so they will survive a teardown
	auto refresh = [englishBtn, deviceComboBox, devHint, downloadSizeLabel, neuralDeviceId]() {
		const std::string id = deviceComboBox->currentData().toString().toStdString();
		const bool isNeuralEngine = !neuralDeviceId.empty() && id == neuralDeviceId;
		if (isNeuralEngine) {
			devHint->setText(
				QStringLiteral("Runs the model on the Neural Engine, leaving your graphics card free. "
					       "Needs a one-time extra download, included below."));
		} else if (id == "cpu") {
			devHint->setText(QStringLiteral(
				"The CPU will work, but may not be powerful enough to keep up with live speech."));
		} else {
			devHint->setText(QStringLiteral(
				"Pick your most powerful graphics card - that is what the model runs on. "
				"It shares the card with OBS's own rendering."));
		}
		const StandardModel &model = englishBtn->isChecked() ? kEnglishModel : kMultilingualModel;
		downloadSizeLabel->setText(QStringLiteral("Total download: %1 (%2)")
						   .arg(humanSize(totalDownloadBytes(model, isNeuralEngine)))
						   .arg(QString::fromUtf8(model.filename)));
	};

	// set up the interactive choices
	QObject::connect(englishBtn, &QRadioButton::toggled, &dlg, [refresh](bool) { refresh(); });
	QObject::connect(deviceComboBox, &QComboBox::currentIndexChanged, &dlg, [refresh](int) { refresh(); });
	refresh();

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dlg);
	QPushButton *goButton = buttons->addButton(QStringLiteral("Download"), QDialogButtonBox::AcceptRole);
	goButton->setDefault(true);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	root->addWidget(buttons);

	if (dlg.exec() != QDialog::Accepted)
		return chooserResult;

	chooserResult.accepted = true;
	chooserResult.englishOnly = englishBtn->isChecked();
	chooserResult.deviceId = deviceComboBox->currentData().toString().toStdString();
	return chooserResult;
}

} // end of  namespace

/*
    This is where the download of the standard model happens. The function orchestrates the various
    dialogues as follows:
 
    - if the user needs an explanation (usually on first download) then pop this up as a modal
    - put up the model choice options dialogue and get the result
    - download the matching standard model
    - if required then download the matching core ML encoder
 */
void downloadStandardModelGuided(const std::vector<ProcessingDeviceOption> &devices, const std::string &currentDevice,
				 const std::string &neuralDeviceId, bool explainFirst,
				 std::function<void(const StandardModelOutcome &)> onComplete)
{
	StandardModelOutcome outcome;
	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());

	// Handle the explanation dialogue
	if (explainFirst) {
		const auto answer = QMessageBox::question(
			parent, QStringLiteral("BabelStreamer"),
			QStringLiteral("BabelStreamer has no speech model yet, so it cannot caption "
				       "anything.\n\nDownload one now?"),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
		if (answer != QMessageBox::Yes) {
			onComplete(outcome);
			return;
		}
	}

	// Show the chooser dialogue modally. If aborted exit the process
	const ChooserResult choice = askForModelChoice(parent, devices, currentDevice, neuralDeviceId);
	if (!choice.accepted) {
		onComplete(outcome);
		return;
	}
	outcome.accepted = true;
	outcome.englishOnly = choice.englishOnly;
	outcome.deviceId = choice.deviceId;

	// Now get the model and handle success/failure
	const StandardModel &model = choice.englishOnly ? kEnglishModel : kMultilingualModel;
	downloadModelFile(model, [&outcome](const std::string &path) { outcome.modelPath = path; });

	if (outcome.modelPath.empty()) {
		onComplete(outcome); // cancelled or failed
		return;
	}

	// If we're using core ML we need to get the core ML encoder. If this fails the
	// device will fall back to CPU or GPU
	if (!neuralDeviceId.empty() && outcome.deviceId == neuralDeviceId) {
		CoremlEncoderResult result = CoremlEncoderResult::Failed;
		downloadCoremlEncoderFor(outcome.modelPath, /*askFirst=*/false,
					 [&result](CoremlEncoderResult r) { result = r; });
		if (result != CoremlEncoderResult::Installed) {
			QMessageBox::warning(parent, QStringLiteral("BabelStreamer"),
					     QStringLiteral(
						     "The model was installed, but its Neural Engine support was not. "
						     "Speech will be recognised on the CPU until that download "
						     "succeeds - use \"Download Neural Engine Model\" to retry."));
		}
	}

	// inform via the callback
	onComplete(outcome);
}
