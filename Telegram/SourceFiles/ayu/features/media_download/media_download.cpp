// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/media_download/media_download.h"

#include "base/base_file_utilities.h"
#include "base/unixtime.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/facade.h"
#include "mtproto/mtp_instance.h"
#include "settings.h"
#include "storage/download_manager_mtproto.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QStorageInfo>

namespace AyuMediaDownload {
namespace {

constexpr auto kPageLimit = 100;
constexpr auto kQueueRefill = 64;
constexpr auto kQueueScan = 16;
constexpr auto kMaxAttempts = 3;
constexpr auto kPageDelay = crl::time(500);
constexpr auto kSaveDelay = crl::time(1000);
constexpr auto kPremiumHintDuration = crl::time(10000);
constexpr auto kLargeFileSize = int64(20 * 1024 * 1024);
constexpr auto kSpaceReserve = int64(64 * 1024 * 1024);
constexpr auto kFallbackLimit = 1;

[[nodiscard]] MTPMessagesFilter Filter(MediaType type) {
	switch (type) {
	case MediaType::Photo: return MTP_inputMessagesFilterPhotos();
	case MediaType::Video: return MTP_inputMessagesFilterVideo();
	case MediaType::File: return MTP_inputMessagesFilterDocument();
	case MediaType::Music: return MTP_inputMessagesFilterMusic();
	case MediaType::Voice: return MTP_inputMessagesFilterVoice();
	case MediaType::Round: return MTP_inputMessagesFilterRoundVideo();
	}
	Unexpected("Type in AyuMediaDownload::Filter.");
}

[[nodiscard]] QString Prefix(MediaType type) {
	switch (type) {
	case MediaType::Photo: return u"photo"_q;
	case MediaType::Video: return u"video"_q;
	case MediaType::File: return u"file"_q;
	case MediaType::Music: return u"audio"_q;
	case MediaType::Voice: return u"voice"_q;
	case MediaType::Round: return u"round"_q;
	}
	Unexpected("Type in AyuMediaDownload::Prefix.");
}

[[nodiscard]] QString DefaultExtension(not_null<DocumentData*> document) {
	const auto patterns = Core::MimeTypeForName(
		document->mimeString()).globPatterns();
	if (!patterns.isEmpty()) {
		auto pattern = patterns.front();
		return pattern.replace('*', QString());
	} else if (document->isVoiceMessage()) {
		return u".ogg"_q;
	} else if (document->isVideoFile() || document->isVideoMessage()) {
		return u".mp4"_q;
	}
	return QString();
}

[[nodiscard]] bool CompleteFile(const QString &path, int64 size) {
	const auto info = QFileInfo(path);
	return info.isFile() && (size ? (info.size() == size) : (info.size() > 0));
}

[[nodiscard]] bool ClaimPath(const QString &path) {
	auto file = QFile(path);
	return file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
}

struct PathOwner {
	const void *job = nullptr;
	QString key;

	friend inline bool operator==(
		const PathOwner &,
		const PathOwner &) = default;
};

auto ReservedPaths = base::flat_map<QString, PathOwner>();

auto Instances = base::flat_map<
	not_null<Main::Session*>,
	std::unique_ptr<Manager>>();

[[nodiscard]] QString ReservationKey(const QString &path) {
	return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
}

[[nodiscard]] bool ReservedByOther(
		const PathOwner &owner,
		const QString &path) {
	const auto i = ReservedPaths.find(ReservationKey(path));
	return (i != end(ReservedPaths)) && !(i->second == owner);
}

[[nodiscard]] bool Reserve(const PathOwner &owner, const QString &path) {
	if (ReservedByOther(owner, path)) {
		return false;
	}
	ReservedPaths[ReservationKey(path)] = owner;
	return true;
}

void Release(const PathOwner &owner, const QString &path) {
	const auto i = ReservedPaths.find(ReservationKey(path));
	if (i != end(ReservedPaths) && i->second == owner) {
		ReservedPaths.erase(i);
	}
}

void ReleaseAll(const void *job) {
	for (auto i = begin(ReservedPaths); i != end(ReservedPaths);) {
		if (i->second.job == job) {
			i = ReservedPaths.erase(i);
		} else {
			++i;
		}
	}
}

void SetFileTime(const QString &path, TimeId date) {
	if (date <= 0) {
		return;
	}
	auto file = QFile(path);
	if (file.open(QIODevice::ReadWrite)) {
		const auto when = base::unixtime::parse(date);
		file.setFileTime(when, QFileDevice::FileModificationTime);
		file.setFileTime(when, QFileDevice::FileAccessTime);
	}
}

[[nodiscard]] QString StatusToString(JobStatus status) {
	switch (status) {
	case JobStatus::Running: return u"running"_q;
	case JobStatus::Paused: return u"paused"_q;
	case JobStatus::Finished: return u"finished"_q;
	case JobStatus::Failed: return u"failed"_q;
	}
	return u"paused"_q;
}

[[nodiscard]] QString PeerToString(PeerId peerId) {
	return QString::number(SerializePeerId(peerId));
}

[[nodiscard]] PeerId PeerFromString(const QJsonValue &value) {
	return DeserializePeerId(value.toString().toULongLong());
}

[[nodiscard]] QJsonArray ToArray(const base::flat_set<QString> &set) {
	auto result = QJsonArray();
	for (const auto &value : set) {
		result.push_back(value);
	}
	return result;
}

[[nodiscard]] base::flat_set<QString> FromArray(const QJsonValue &value) {
	auto result = base::flat_set<QString>();
	for (const auto &entry : value.toArray()) {
		result.emplace(entry.toString());
	}
	return result;
}

} // namespace

const std::vector<MediaType> &AllMediaTypes() {
	static const auto result = std::vector<MediaType>{
		MediaType::Photo,
		MediaType::Video,
		MediaType::File,
		MediaType::Music,
		MediaType::Voice,
		MediaType::Round,
	};
	return result;
}

Slots::Slots(not_null<Main::Session*> session)
: _session(session)
, _api(&session->mtp()) {
}

void Slots::requestLimits() {
	if (_requested) {
		return;
	}
	_requested = true;
	_api.request(MTPaccount_GetAutoDownloadSettings(
	)).done([=](const MTPaccount_AutoDownloadSettings &result) {
		const auto &medium = result.data().vmedium().data();
		_smallLimit = std::max(
			medium.vsmall_queue_active_operations_max().v,
			kFallbackLimit);
		_largeLimit = std::max(
			medium.vlarge_queue_active_operations_max().v,
			kFallbackLimit);
		_available.fire({});
	}).fail([=](const MTP::Error &error) {
		LOG(("AyuMediaDownload: auto download settings failed, %1"
			).arg(error.type()));
		_smallLimit = _largeLimit = kFallbackLimit;
		_available.fire({});
	}).send();
}

bool Slots::tryAcquire(int32 dcId, bool large) {
	const auto key = std::make_pair(dcId, large);
	const auto i = _active.find(key);
	const auto count = (i != end(_active)) ? i->second : 0;
	if (count >= (large ? _largeLimit : _smallLimit)) {
		return false;
	}
	++_active[key];
	return true;
}

void Slots::release(int32 dcId, bool large) {
	const auto i = _active.find(std::make_pair(dcId, large));
	if (i == end(_active)) {
		return;
	} else if (--i->second <= 0) {
		_active.erase(i);
	}
	_available.fire({});
}

rpl::producer<> Slots::available() const {
	return _available.events();
}

Job::Job(
	not_null<Main::Session*> session,
	not_null<Slots*> slots,
	JobSettings settings,
	Fn<void()> changed)
: _session(session)
, _slots(slots)
, _settings(std::move(settings))
, _changed(std::move(changed))
, _api(&session->mtp())
, _discoveryTimer([=] { fillSlots(); })
, _sweepTimer([=] { sweep(); }) {
	const auto addCursors = [&](PeerId peerId) {
		for (const auto type : AllMediaTypes()) {
			if (_settings.types & type) {
				_cursors.push_back({ .peerId = peerId, .type = type });
			}
		}
	};
	addCursors(_settings.peerId);
	if (_settings.migratedPeerId && !_settings.topicRootId) {
		addCursors(_settings.migratedPeerId);
	}

	_session->data().documentLoadProgress(
	) | rpl::on_next([=](not_null<DocumentData*> document) {
		const auto ours = ranges::any_of(_active, [&](const Active &a) {
			return (a.entry.document == document.get());
		});
		if (ours) {
			_sweepTimer.callOnce(0);
		}
	}, _lifetime);

	_session->data().photoLoadProgress(
	) | rpl::on_next([=](not_null<PhotoData*> photo) {
		const auto ours = ranges::any_of(_active, [&](const Active &a) {
			return (a.entry.photo == photo.get());
		});
		if (ours) {
			_sweepTimer.callOnce(0);
		}
	}, _lifetime);

	_session->downloader().nonPremiumDelays(
	) | rpl::on_next([=](DocumentId id) {
		const auto ours = ranges::any_of(_active, [&](const Active &a) {
			return a.entry.document && (a.entry.document->id == id);
		});
		if (ours) {
			_premiumLimitedTill = crl::now() + kPremiumHintDuration;
			notify();
		}
	}, _lifetime);

	_session->mtp().floodWaits(
	) | rpl::filter([=](const MTP::FloodWait &wait) {
		return (_status == JobStatus::Running)
			&& !_active.empty()
			&& MTP::isDownloadDcId(wait.shiftedDcId);
	}) | rpl::on_next([=](const MTP::FloodWait &wait) {
		downloadFloodWait(wait.seconds);
	}, _lifetime);

	_slots->available(
	) | rpl::filter([=] {
		return (_status == JobStatus::Running) && !_queue.empty();
	}) | rpl::on_next([=] {
		_sweepTimer.callOnce(0);
	}, _lifetime);
}

Job::~Job() {
	ReleaseAll(this);
}

const JobSettings &Job::settings() const {
	return _settings;
}

JobProgress Job::progress() const {
	const auto now = base::unixtime::now();
	const auto waitUntil = std::max(_waitUntil, _downloadWaitUntil);
	return {
		.status = _status,
		.error = _error,
		.errorDetails = _errorDetails,
		.found = _found,
		.done = int(_completed.size()),
		.pending = int(_queue.size() + _active.size()),
		.unavailable = _unavailableCount,
		.discovering = (_status == JobStatus::Running)
			&& ranges::any_of(_cursors, [](const Cursor &c) {
				return !c.exhausted;
			}),
		.waitUntil = (waitUntil > now) ? waitUntil : 0,
		.premiumLimited = (_premiumLimitedTill > crl::now()),
	};
}

rpl::producer<> Job::updates() const {
	return _updates.events();
}

void Job::notify() {
	_updates.fire({});
	if (_changed) {
		_changed();
	}
}

void Job::start() {
	if (_status == JobStatus::Running || _status == JobStatus::Finished) {
		return;
	}
	if (!QDir().mkpath(_settings.folder)) {
		fail(JobError::FolderUnavailable, _settings.folder);
		return;
	}
	_status = JobStatus::Running;
	_error = JobError::None;
	_errorDetails = QString();
	_slots->requestLimits();
	const auto now = base::unixtime::now();
	if (_waitUntil > now) {
		_discoveryTimer.callOnce(crl::time(_waitUntil - now) * 1000);
	}
	notify();
	_sweepTimer.callOnce(0);
}

void Job::pause() {
	if (_status != JobStatus::Running) {
		return;
	}
	_status = JobStatus::Paused;
	_api.request(base::take(_searchRequestId)).cancel();
	_discoveryTimer.cancel();
	stopActive();
	notify();
}

void Job::stopActive() {
	auto active = base::take(_active);
	for (auto i = active.rbegin(); i != active.rend(); ++i) {
		_slots->release(i->dcId, i->large);
		if (const auto document = i->entry.document) {
			if (document->loading()
				&& document->loadingFilePath() == i->path) {
				document->cancel();
			}
		} else if (const auto photo = i->entry.photo) {
			const auto onlyUser = i->photoView
				&& (i->photoView.use_count() == 1);
			i->photoView = nullptr;
			if (onlyUser && photo->loading()) {
				photo->cancel();
			}
		}
		_queue.push_front(std::move(i->entry));
	}
}

void Job::fail(JobError error, const QString &details) {
	_status = JobStatus::Failed;
	_error = error;
	_errorDetails = details;
	_api.request(base::take(_searchRequestId)).cancel();
	_discoveryTimer.cancel();
	stopActive();
	notify();
}

Job::Cursor *Job::currentCursor() {
	const auto i = ranges::find(_cursors, false, &Cursor::exhausted);
	return (i != end(_cursors)) ? &*i : nullptr;
}

void Job::requestNextPage() {
	if (_status != JobStatus::Running
		|| _searchRequestId
		|| _discoveryTimer.isActive()
		|| int(_queue.size()) >= kQueueRefill) {
		return;
	}
	const auto now = base::unixtime::now();
	if (_waitUntil > now) {
		_discoveryTimer.callOnce(crl::time(_waitUntil - now) * 1000);
		return;
	}
	const auto cursor = currentCursor();
	if (!cursor) {
		return;
	}
	const auto peer = _session->data().peerLoaded(cursor->peerId);
	if (!peer) {
		fail(JobError::Access);
		return;
	}
	const auto index = int(cursor - _cursors.data());
	const auto topicRootId = (cursor->peerId == _settings.peerId)
		? _settings.topicRootId
		: MsgId();
	using Flag = MTPmessages_Search::Flag;
	_searchRequestId = _api.request(MTPmessages_Search(
		MTP_flags(topicRootId ? Flag::f_top_msg_id : Flag(0)),
		peer->input(),
		MTP_string(),
		MTP_inputPeerEmpty(),
		MTPInputPeer(), // saved_peer_id
		MTPVector<MTPReaction>(), // saved_reaction
		MTP_int(int(topicRootId.bare)),
		Filter(cursor->type),
		MTP_int(_settings.minDate),
		MTP_int(_settings.maxDate),
		MTP_int(int(cursor->offsetId.bare)),
		MTP_int(0), // add_offset
		MTP_int(kPageLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		_searchRequestId = 0;
		processPage(index, result);
		_discoveryTimer.callOnce(kPageDelay);
		notify();
		_sweepTimer.callOnce(0);
	}).fail([=](const MTP::Error &error) {
		_searchRequestId = 0;
		discoveryFailed(error);
	}).handleFloodErrors().send();
	notify();
}

void Job::discoveryFailed(const MTP::Error &error) {
	const auto &type = error.type();
	if (type.startsWith(u"FLOOD_WAIT_"_q)
		|| type.startsWith(u"FLOOD_PREMIUM_WAIT_"_q)) {
		const auto seconds = std::max(
			type.mid(type.lastIndexOf('_') + 1).toInt(),
			1);
		_waitUntil = base::unixtime::now() + seconds;
		_discoveryTimer.callOnce(crl::time(seconds) * 1000);
		notify();
		return;
	}
	LOG(("AyuMediaDownload: search failed, %1").arg(type));
	fail(JobError::Access, type);
}

void Job::downloadFloodWait(int seconds) {
	const auto until = base::unixtime::now() + std::max(seconds, 1);
	if (until > _downloadWaitUntil) {
		_downloadWaitUntil = until;
		notify();
	}
}

void Job::processPage(int cursorIndex, const MTPmessages_Messages &result) {
	if (cursorIndex < 0 || cursorIndex >= int(_cursors.size())) {
		return;
	}
	auto &cursor = _cursors[cursorIndex];
	const auto messages = result.match([](
			const MTPDmessages_messagesNotModified &) {
		return QVector<MTPMessage>();
	}, [&](const auto &data) {
		_session->data().processUsers(data.vusers());
		_session->data().processChats(data.vchats());
		return data.vmessages().v;
	});
	if (messages.isEmpty()) {
		cursor.exhausted = true;
		return;
	}
	auto minId = cursor.offsetId;
	for (const auto &message : messages) {
		const auto id = IdFromMessage(message);
		if (!minId || id < minId) {
			minId = id;
		}
		processMessage(cursor.peerId, cursor.type, message);
	}
	if (minId == cursor.offsetId) {
		cursor.exhausted = true;
	} else {
		cursor.offsetId = minId;
	}
}

void Job::processMessage(
		PeerId peerId,
		MediaType type,
		const MTPMessage &message) {
	message.match([&](const MTPDmessage &data) {
		const auto media = data.vmedia();
		if (!media) {
			return;
		}
		auto entry = Entry{
			.peerId = peerId,
			.msgId = MsgId(data.vid().v),
			.type = type,
			.date = data.vdate().v,
		};
		auto missing = false;
		media->match([&](const MTPDmessageMediaPhoto &photoMedia) {
			const auto photo = photoMedia.vphoto();
			if (photoMedia.vttl_seconds()) {
				return;
			} else if (!photo || photo->type() != mtpc_photo) {
				missing = true;
				return;
			}
			entry.photo = _session->data().processPhoto(*photo);
			entry.key = u"p"_q + QString::number(entry.photo->id);
		}, [&](const MTPDmessageMediaDocument &documentMedia) {
			const auto document = documentMedia.vdocument();
			if (documentMedia.vttl_seconds()) {
				return;
			} else if (!document || document->type() != mtpc_document) {
				missing = true;
				return;
			}
			entry.document = _session->data().processDocument(*document);
			entry.key = u"d"_q + QString::number(entry.document->id);
		}, [](const auto &) {
		});
		if (missing) {
			markUnavailable(u"m"_q
				+ PeerToString(peerId)
				+ '_'
				+ QString::number(entry.msgId.bare));
			return;
		} else if (entry.key.isEmpty()
			|| _seen.contains(entry.key)
			|| _completed.contains(entry.key)
			|| _unavailable.contains(entry.key)) {
			return;
		}
		_seen.emplace(entry.key);
		++_found;
		_queue.push_back(std::move(entry));
	}, [](const auto &) {
	});
}

bool Job::isLarge(const Entry &entry) const {
	return entry.document && (entry.document->size >= kLargeFileSize);
}

int32 Job::dcId(const Entry &entry) const {
	return entry.document
		? entry.document->getDC()
		: entry.photo
		? entry.photo->getDC()
		: 0;
}

bool Job::downloadsBlocked() const {
	return (_downloadWaitUntil > base::unixtime::now());
}

JobError Job::localProblem(int64 size) const {
	const auto folder = QFileInfo(_settings.folder);
	const auto storage = QStorageInfo(_settings.folder);
	if (!folder.isDir()
		|| !folder.isWritable()
		|| !storage.isValid()
		|| !storage.isReady()) {
		return JobError::FolderUnavailable;
	} else if (storage.bytesAvailable() < size + kSpaceReserve) {
		return JobError::NoSpace;
	}
	return JobError::None;
}

Job::Outcome Job::checkDocument(Active &active) {
	const auto document = active.entry.document;
	const auto ownLoader = (document->loadingFilePath() == active.path);
	if (document->loading()) {
		if (!active.started
			&& !ownLoader
			&& document->loadingFilePath().isEmpty()
			&& !downloadsBlocked()) {
			active.started = true;
			document->save(origin(active.entry), active.path);
		}
		return Outcome::Pending;
	}
	const auto loaded = document->filepath(true);
	if (loaded == active.path
		&& !document->cancelled()
		&& document->status != FileDownloadFailed
		&& CompleteFile(active.path, document->size)) {
		return Outcome::Done;
	} else if (!loaded.isEmpty()
		&& loaded != active.path
		&& CompleteFile(loaded, document->size)) {
		QFile::remove(active.path);
		return QFile::copy(loaded, active.path)
			? Outcome::Done
			: Outcome::LocalError;
	}
	if (!active.started) {
		if (downloadsBlocked()) {
			return Outcome::Pending;
		}
		active.started = true;
		document->save(origin(active.entry), active.path);
		_sweepTimer.callOnce(0);
		return Outcome::Pending;
	}
	return (localProblem(document->size) != JobError::None)
		? Outcome::LocalError
		: Outcome::Retry;
}

Job::Outcome Job::checkPhoto(Active &active) {
	const auto photo = active.entry.photo;
	if (!active.photoView) {
		if (downloadsBlocked()) {
			return Outcome::Pending;
		}
		active.started = true;
		photo->clearFailed(Data::PhotoSize::Large);
		active.photoView = photo->createMediaView();
		active.photoView->wanted(
			Data::PhotoSize::Large,
			origin(active.entry));
		_sweepTimer.callOnce(0);
		return Outcome::Pending;
	} else if (active.photoView->loaded()) {
		return active.photoView->saveToFile(active.path)
			? Outcome::Done
			: Outcome::LocalError;
	} else if (photo->failed(Data::PhotoSize::Large)) {
		return Outcome::Retry;
	}
	return Outcome::Pending;
}

void Job::sweep() {
	auto finished = std::vector<std::pair<QString, Outcome>>();
	for (auto &active : _active) {
		const auto outcome = active.entry.document
			? checkDocument(active)
			: checkPhoto(active);
		if (outcome != Outcome::Pending) {
			finished.emplace_back(active.entry.key, outcome);
		}
	}
	for (const auto &[key, outcome] : finished) {
		finishActive(key, outcome);
	}
	fillSlots();
}

void Job::fillSlots() {
	if (_status != JobStatus::Running) {
		return;
	}
	const auto now = base::unixtime::now();
	if (_downloadWaitUntil > now) {
		_sweepTimer.callOnce(crl::time(_downloadWaitUntil - now) * 1000);
	} else {
		auto index = 0;
		while (index < int(_queue.size()) && index < kQueueScan) {
			const auto &front = _queue[index];
			if (!_slots->tryAcquire(dcId(front), isLarge(front))) {
				++index;
				continue;
			}
			auto entry = std::move(_queue[index]);
			_queue.erase(begin(_queue) + index);
			if (!startEntry(std::move(entry))) {
				return;
			}
		}
	}
	requestNextPage();
	checkFinished();
}

bool Job::startEntry(Entry entry) {
	const auto size = entry.document ? entry.document->size : int64(0);
	const auto dc = dcId(entry);
	const auto large = isLarge(entry);
	const auto problem = localProblem(size);
	const auto path = (problem == JobError::None)
		? choosePath(entry)
		: QString();
	if (path.isEmpty()) {
		_slots->release(dc, large);
		_queue.push_front(std::move(entry));
		fail(
			(problem != JobError::None)
				? problem
				: JobError::FolderUnavailable,
			_settings.folder);
		return false;
	}
	_reservedPaths[entry.key] = path;
	_active.push_back({
		.entry = std::move(entry),
		.path = path,
		.dcId = dc,
		.large = large,
	});
	_sweepTimer.callOnce(0);
	return true;
}

void Job::markUnavailable(const QString &key) {
	if (_unavailable.emplace(key).second) {
		++_unavailableCount;
	}
}

void Job::finishActive(const QString &key, Outcome outcome) {
	const auto i = ranges::find(_active, key, [](const Active &a) {
		return a.entry.key;
	});
	if (i == end(_active)) {
		return;
	}
	auto active = std::move(*i);
	_active.erase(i);
	_slots->release(active.dcId, active.large);
	switch (outcome) {
	case Outcome::Pending:
		break;
	case Outcome::Done:
		_reservedPaths.remove(key);
		Release({ this, key }, active.path);
		SetFileTime(active.path, active.entry.date);
		_completed.emplace(key);
		break;
	case Outcome::Retry:
		if (++active.entry.attempts < kMaxAttempts) {
			_queue.push_back(std::move(active.entry));
		} else {
			_reservedPaths.remove(key);
			Release({ this, key }, active.path);
			markUnavailable(key);
			QFile::remove(active.path);
		}
		break;
	case Outcome::LocalError: {
		const auto size = active.entry.document
			? active.entry.document->size
			: int64(0);
		_queue.push_front(std::move(active.entry));
		const auto problem = localProblem(size);
		fail(
			(problem != JobError::None)
				? problem
				: JobError::FolderUnavailable,
			active.path);
	} break;
	}
	notify();
}

void Job::checkFinished() {
	if (_status != JobStatus::Running
		|| !_queue.empty()
		|| !_active.empty()
		|| _searchRequestId
		|| currentCursor()) {
		return;
	}
	_status = JobStatus::Finished;
	_discoveryTimer.cancel();
	notify();
}

QString Job::choosePath(const Entry &entry) {
	const auto owner = PathOwner{ .job = this, .key = entry.key };
	const auto reserved = _reservedPaths.find(entry.key);
	if (reserved != end(_reservedPaths)) {
		const auto path = reserved->second;
		if (Reserve(owner, path)
			&& (QFileInfo::exists(path) || ClaimPath(path))) {
			return path;
		}
		Release(owner, path);
		_reservedPaths.erase(reserved);
	}
	const auto when = base::unixtime::parse(entry.date).toString(
		u"yyyy-MM-dd_HH-mm-ss"_q);
	const auto fallback = Prefix(entry.type)
		+ '_'
		+ when
		+ '_'
		+ QString::number(entry.msgId.bare);
	auto name = QString();
	auto extension = QString();
	if (entry.photo) {
		name = fallback;
		extension = u".jpg"_q;
	} else {
		const auto original = base::FileNameFromUserString(
			entry.document->filename());
		const auto dot = original.lastIndexOf('.');
		if (original.isEmpty()) {
			name = fallback;
			extension = DefaultExtension(entry.document);
		} else if (dot > 0) {
			name = original.mid(0, dot);
			extension = original.mid(dot);
		} else {
			name = original;
		}
	}
	const auto dir = QDir(_settings.folder);
	auto failed = false;
	const auto attempt = [&](const QString &fileName) {
		const auto path = dir.filePath(fileName + extension);
		if (ReservedByOther(owner, path)) {
			return QString();
		} else if (ClaimPath(path)) {
			ReservedPaths[ReservationKey(path)] = owner;
			return path;
		} else if (!QFileInfo::exists(path)) {
			failed = true;
		}
		return QString();
	};
	const auto plain = attempt(name);
	if (!plain.isEmpty() || failed) {
		return plain;
	}
	const auto unique = name + u" ("_q + entry.key + ')';
	for (auto i = 0; !failed; ++i) {
		const auto result = attempt(i
			? (unique + u" ("_q + QString::number(i) + ')')
			: unique);
		if (!result.isEmpty()) {
			return result;
		}
	}
	return QString();
}

void Job::discardReserved() {
	for (const auto &[key, path] : base::take(_reservedPaths)) {
		Release({ this, key }, path);
		QFile::remove(path);
	}
}

Data::FileOrigin Job::origin(const Entry &entry) const {
	return Data::FileOrigin(FullMsgId(entry.peerId, entry.msgId));
}

QByteArray Job::serialize() const {
	auto cursors = QJsonArray();
	for (const auto &cursor : _cursors) {
		auto offsetId = cursor.offsetId;
		auto exhausted = cursor.exhausted;
		auto pendingMax = MsgId();
		const auto check = [&](const Entry &entry) {
			if (entry.peerId == cursor.peerId
				&& entry.type == cursor.type
				&& entry.msgId > pendingMax) {
				pendingMax = entry.msgId;
			}
		};
		ranges::for_each(_queue, check);
		for (const auto &active : _active) {
			check(active.entry);
		}
		if (pendingMax) {
			offsetId = MsgId(pendingMax.bare + 1);
			exhausted = false;
		}
		cursors.push_back(QJsonObject{
			{ u"peer"_q, PeerToString(cursor.peerId) },
			{ u"type"_q, int(cursor.type) },
			{ u"offset"_q, QString::number(offsetId.bare) },
			{ u"exhausted"_q, exhausted },
		});
	}
	auto reserved = QJsonObject();
	for (const auto &[key, path] : _reservedPaths) {
		reserved.insert(key, path);
	}
	const auto pending = int(_queue.size() + _active.size());
	const auto status = (_status == JobStatus::Running)
		? JobStatus::Paused
		: _status;
	const auto object = QJsonObject{
		{ u"peer"_q, PeerToString(_settings.peerId) },
		{ u"migrated"_q, PeerToString(_settings.migratedPeerId) },
		{ u"topic"_q, QString::number(_settings.topicRootId.bare) },
		{ u"folder"_q, _settings.folder },
		{ u"types"_q, int(_settings.types.value()) },
		{ u"minDate"_q, _settings.minDate },
		{ u"maxDate"_q, _settings.maxDate },
		{ u"status"_q, StatusToString(status) },
		{ u"found"_q, std::max(_found - pending, 0) },
		{ u"unavailableCount"_q, _unavailableCount },
		{ u"waitUntil"_q, QString::number(_waitUntil) },
		{ u"downloadWaitUntil"_q, QString::number(_downloadWaitUntil) },
		{ u"cursors"_q, cursors },
		{ u"completed"_q, ToArray(_completed) },
		{ u"unavailable"_q, ToArray(_unavailable) },
		{ u"reserved"_q, reserved },
	};
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::unique_ptr<Job> Job::Deserialize(
		not_null<Main::Session*> session,
		not_null<Slots*> slots,
		const QByteArray &data,
		Fn<void()> changed) {
	const auto document = QJsonDocument::fromJson(data);
	if (!document.isObject()) {
		return nullptr;
	}
	const auto object = document.object();
	auto settings = JobSettings{
		.peerId = PeerFromString(object.value(u"peer"_q)),
		.migratedPeerId = PeerFromString(object.value(u"migrated"_q)),
		.topicRootId = MsgId(
			object.value(u"topic"_q).toString().toLongLong()),
		.folder = object.value(u"folder"_q).toString(),
		.types = MediaTypes::from_raw(
			uint8(object.value(u"types"_q).toInt())),
		.minDate = TimeId(object.value(u"minDate"_q).toInt()),
		.maxDate = TimeId(object.value(u"maxDate"_q).toInt()),
	};
	if (!settings.peerId || settings.folder.isEmpty() || !settings.types) {
		return nullptr;
	}
	auto result = std::make_unique<Job>(
		session,
		slots,
		std::move(settings),
		std::move(changed));
	const auto status = object.value(u"status"_q).toString();
	result->_status = (status == u"finished"_q)
		? JobStatus::Finished
		: JobStatus::Paused;
	result->_found = object.value(u"found"_q).toInt();
	result->_unavailableCount = object.value(u"unavailableCount"_q).toInt();
	result->_waitUntil = TimeId(
		object.value(u"waitUntil"_q).toString().toLongLong());
	result->_downloadWaitUntil = TimeId(
		object.value(u"downloadWaitUntil"_q).toString().toLongLong());
	for (const auto &value : object.value(u"cursors"_q).toArray()) {
		const auto cursor = value.toObject();
		const auto peerId = PeerFromString(cursor.value(u"peer"_q));
		const auto type = MediaType(cursor.value(u"type"_q).toInt());
		const auto i = ranges::find_if(result->_cursors, [&](
				const Cursor &existing) {
			return (existing.peerId == peerId) && (existing.type == type);
		});
		if (i != end(result->_cursors)) {
			i->offsetId = MsgId(
				cursor.value(u"offset"_q).toString().toLongLong());
			i->exhausted = cursor.value(u"exhausted"_q).toBool();
		}
	}
	result->_completed = FromArray(object.value(u"completed"_q));
	result->_unavailable = FromArray(object.value(u"unavailable"_q));
	const auto reserved = object.value(u"reserved"_q).toObject();
	for (auto i = reserved.begin(); i != reserved.end(); ++i) {
		const auto path = i.value().toString();
		if (Reserve({ result.get(), i.key() }, path)) {
			result->_reservedPaths.emplace(i.key(), path);
		}
	}
	return result;
}

Manager::Manager(not_null<Main::Session*> session)
: _session(session)
, _slots(session)
, _saveTimer([=] { save(); }) {
	load();
}

Manager::~Manager() {
	save();
}

not_null<Manager*> Manager::Instance(not_null<Main::Session*> session) {
	const auto i = Instances.find(session);
	if (i != end(Instances)) {
		return i->second.get();
	}
	const auto result = Instances.emplace(
		session,
		std::make_unique<Manager>(session)).first->second.get();
	const auto destroy = [=] {
		Instances.remove(session);
	};
	session->account().sessionChanges(
	) | rpl::filter([=](Main::Session *value) {
		return (value != session.get());
	}) | rpl::take(1) | rpl::on_next([=](Main::Session*) {
		destroy();
	}, session->lifetime());
	session->lifetime().add(destroy);
	return result;
}

Job *Manager::find(PeerId peerId, MsgId topicRootId) const {
	const auto i = ranges::find_if(_jobs, [&](const auto &job) {
		return (job->settings().peerId == peerId)
			&& (job->settings().topicRootId == topicRootId);
	});
	return (i != end(_jobs)) ? i->get() : nullptr;
}

not_null<Job*> Manager::create(JobSettings settings) {
	if (const auto existing = find(settings.peerId, settings.topicRootId)) {
		remove(existing);
	}
	_jobs.push_back(std::make_unique<Job>(
		_session,
		&_slots,
		std::move(settings),
		[=] { saveDelayed(); }));
	saveDelayed();
	return _jobs.back().get();
}

void Manager::remove(not_null<Job*> job) {
	job->pause();
	job->discardReserved();
	const auto i = ranges::find_if(_jobs, [&](const auto &entry) {
		return (entry.get() == job.get());
	});
	if (i != end(_jobs)) {
		_jobs.erase(i);
	}
	save();
}

QString Manager::path() const {
	return cWorkingDir()
		+ u"tdata/ayu_media_downloads_%1.json"_q.arg(
			QString::number(_session->userId().bare));
}

void Manager::load() {
	auto file = QFile(path());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	for (const auto &value : document.array()) {
		const auto data = QJsonDocument(value.toObject()).toJson(
			QJsonDocument::Compact);
		auto job = Job::Deserialize(
			_session,
			&_slots,
			data,
			[=] { saveDelayed(); });
		if (job) {
			_jobs.push_back(std::move(job));
		}
	}
}

void Manager::saveDelayed() {
	if (!_saveTimer.isActive()) {
		_saveTimer.callOnce(kSaveDelay);
	}
}

void Manager::save() {
	_saveTimer.cancel();
	if (_jobs.empty()) {
		QFile::remove(path());
		return;
	}
	auto array = QJsonArray();
	for (const auto &job : _jobs) {
		array.push_back(QJsonDocument::fromJson(job->serialize()).object());
	}
	auto file = QSaveFile(path());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
		if (!file.commit()) {
			LOG(("AyuMediaDownload: could not write %1").arg(path()));
		}
	}
}

} // namespace AyuMediaDownload
