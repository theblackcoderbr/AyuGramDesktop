// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/boxes/media_download_box.h"

#include "lang_auto.h"
#include "ayu/features/media_download/media_download.h"
#include "base/base_file_utilities.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "ui/boxes/calendar_box.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

namespace AyuUi {
namespace {

using namespace AyuMediaDownload;

[[nodiscard]] QString TypeLabel(MediaType type) {
	switch (type) {
	case MediaType::Photo: return tr::lng_media_type_photos(tr::now);
	case MediaType::Video: return tr::lng_media_type_videos(tr::now);
	case MediaType::File: return tr::lng_media_type_files(tr::now);
	case MediaType::Music: return tr::lng_media_type_songs(tr::now);
	case MediaType::Voice: return tr::lng_media_type_audios(tr::now);
	case MediaType::Round: return tr::lng_media_type_rounds(tr::now);
	}
	Unexpected("Type in AyuUi::TypeLabel.");
}

[[nodiscard]] QString FormatDuration(int seconds) {
	const auto hours = seconds / 3600;
	const auto minutes = (seconds % 3600) / 60;
	const auto secs = seconds % 60;
	const auto two = [](int value) {
		return QString::number(value).rightJustified(2, '0');
	};
	return hours
		? (QString::number(hours) + ':' + two(minutes) + ':' + two(secs))
		: (QString::number(minutes) + ':' + two(secs));
}

[[nodiscard]] QString FormatDate(TimeId date) {
	return QLocale().toString(
		base::unixtime::parse(date).date(),
		QLocale::ShortFormat);
}

[[nodiscard]] QString DefaultFolder(not_null<PeerData*> peer) {
	const auto session = &peer->session();
	const auto saved = Core::App().settings().downloadPath();
	const auto base = (saved.isEmpty() || saved == FileDialog::Tmp())
		? File::DefaultDownloadPath(session)
		: saved;
	auto name = base::FileNameFromUserString(peer->name().simplified());
	if (name.isEmpty()) {
		name = QString::number(peer->id.value);
	}
	return QDir(base).filePath(name);
}

[[nodiscard]] QString ProgressText(const JobProgress &progress) {
	const auto number = [](int value) {
		return QString::number(value);
	};
	auto lines = QStringList();
	switch (progress.status) {
	case JobStatus::Running:
		lines.push_back(tr::ayu_ChatMediaDownloadStatusRunning(tr::now));
		break;
	case JobStatus::Paused:
		lines.push_back(tr::ayu_ChatMediaDownloadStatusPaused(tr::now));
		break;
	case JobStatus::Finished:
		lines.push_back(tr::ayu_ChatMediaDownloadStatusFinished(tr::now));
		break;
	case JobStatus::Failed: {
		const auto reason = [&] {
			switch (progress.error) {
			case JobError::NoSpace:
				return tr::ayu_ChatMediaDownloadErrorNoSpace(tr::now);
			case JobError::FolderUnavailable:
				return tr::ayu_ChatMediaDownloadErrorFolder(tr::now);
			case JobError::Access:
			case JobError::None:
				break;
			}
			return tr::ayu_ChatMediaDownloadErrorAccess(
				tr::now,
				lt_error,
				progress.errorDetails);
		}();
		lines.push_back(tr::ayu_ChatMediaDownloadStatusFailed(
			tr::now,
			lt_reason,
			reason));
	} break;
	}
	lines.push_back(QString());
	lines.push_back((progress.discovering
		? tr::ayu_ChatMediaDownloadSearching
		: tr::ayu_ChatMediaDownloadFound)(
			tr::now,
			lt_number,
			number(progress.found)));
	lines.push_back(tr::ayu_ChatMediaDownloadDone(
		tr::now,
		lt_number,
		number(progress.done)));
	lines.push_back(tr::ayu_ChatMediaDownloadPending(
		tr::now,
		lt_number,
		number(progress.pending)));
	lines.push_back(tr::ayu_ChatMediaDownloadUnavailable(
		tr::now,
		lt_number,
		number(progress.unavailable)));
	if (const auto left = progress.waitUntil - base::unixtime::now()
		; left > 0) {
		lines.push_back(QString());
		lines.push_back(tr::ayu_ChatMediaDownloadWait(
			tr::now,
			lt_duration,
			FormatDuration(left)));
	}
	if (progress.premiumLimited && progress.status == JobStatus::Running) {
		lines.push_back(QString());
		lines.push_back(tr::ayu_ChatMediaDownloadPremiumLimited(tr::now));
	}
	return lines.join('\n');
}

void OpenFolder(const QString &folder) {
	QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void FillProgressBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		PeerId peerId,
		MsgId topicRootId) {
	const auto session = &controller->session();
	const auto manager = Manager::Instance(session);
	const auto job = manager->find(peerId, topicRootId);
	if (!job) {
		box->closeBox();
		return;
	}
	const auto folder = job->settings().folder;
	const auto weak = base::make_weak(job);
	box->setTitle(tr::ayu_ChatMediaDownloadTitle());
	box->setWidth(st::boxWideWidth);

	struct State {
		rpl::variable<QString> text;
		base::Timer ticker;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto refresh = [=] {
		if (const auto strong = weak.get()) {
			state->text = ProgressText(strong->progress());
		}
	};
	state->ticker.setCallback(refresh);
	state->ticker.callEach(1000);
	refresh();
	job->updates() | rpl::on_next(refresh, box->lifetime());

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			state->text.value(),
			st::boxLabel),
		st::boxRowPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			folder,
			st::boxDividerLabel),
		st::boxRowPadding);

	const auto confirmCancel = [=] {
		controller->show(Ui::MakeConfirmBox({
			.text = tr::ayu_ChatMediaDownloadCancelConfirm(),
			.confirmed = [=](Fn<void()> &&close) {
				box->closeBox();
				close();
				if (const auto current = manager->find(peerId, topicRootId)) {
					manager->remove(current);
				}
			},
			.confirmText = tr::ayu_ChatMediaDownloadCancelTask(),
			.confirmStyle = &st::attentionBoxButton,
		}));
	};
	const auto remove = [=] {
		box->closeBox();
		if (const auto current = manager->find(peerId, topicRootId)) {
			manager->remove(current);
		}
	};
	const auto rebuild = [=](JobStatus status) {
		box->clearButtons();
		switch (status) {
		case JobStatus::Running:
			box->addButton(tr::ayu_ChatMediaDownloadPause(), [=] {
				if (const auto strong = weak.get()) {
					strong->pause();
				}
			});
			box->addButton(
				tr::ayu_ChatMediaDownloadCancelTask(),
				confirmCancel);
			break;
		case JobStatus::Paused:
		case JobStatus::Failed:
			box->addButton(tr::ayu_ChatMediaDownloadResume(), [=] {
				if (const auto strong = weak.get()) {
					strong->start();
				}
			});
			box->addButton(
				tr::ayu_ChatMediaDownloadCancelTask(),
				confirmCancel);
			break;
		case JobStatus::Finished:
			box->addButton(tr::lng_close(), [=] { box->closeBox(); });
			box->addButton(tr::ayu_ChatMediaDownloadRemove(), remove);
			break;
		}
		box->addLeftButton(
			tr::ayu_ChatMediaDownloadOpenFolder(),
			[=] { OpenFolder(folder); });
	};
	rpl::single(
		rpl::empty
	) | rpl::then(
		job->updates()
	) | rpl::filter([=] {
		return (weak.get() != nullptr);
	}) | rpl::map([=] {
		return weak.get()->progress().status;
	}) | rpl::distinct_until_changed(
	) | rpl::on_next(rebuild, box->lifetime());
}

void EditDate(
		not_null<Window::SessionController*> controller,
		TimeId current,
		Fn<void(TimeId)> done) {
	const auto highlighted = current
		? base::unixtime::parse(current).date()
		: QDate::currentDate();
	const auto shared = std::make_shared<base::weak_qptr<Ui::CalendarBox>>();
	const auto finalize = [=](not_null<Ui::CalendarBox*> box) {
		box->addLeftButton(tr::ayu_ChatMediaDownloadReset(), [=] {
			done(0);
			if (const auto strong = shared->get()) {
				strong->closeBox();
			}
		});
	};
	auto box = Box<Ui::CalendarBox>(Ui::CalendarBoxArgs{
		.month = highlighted,
		.highlighted = highlighted,
		.callback = [=](const QDate &date, Fn<void()> close) {
			done(base::unixtime::serialize(date.startOfDay()));
			close();
		},
		.finalize = finalize,
		.minDate = QDate(2013, 8, 1),
		.maxDate = QDate::currentDate(),
	});
	*shared = base::make_weak(box.data());
	controller->show(std::move(box));
}

void FillSetupBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId topicRootId) {
	box->setTitle(tr::ayu_ChatMediaDownloadTitle());
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();

	struct State {
		MediaTypes types = MediaType::Photo
			| MediaType::Video
			| MediaType::File;
		rpl::variable<TimeId> from = 0;
		rpl::variable<TimeId> till = 0;
		rpl::variable<QString> folder;
	};
	const auto state = box->lifetime().make_state<State>();
	state->folder = DefaultFolder(peer);

	Ui::AddSubsectionTitle(container, tr::ayu_ChatMediaDownloadTypes());
	for (const auto type : AllMediaTypes()) {
		const auto checkbox = box->addRow(
			object_ptr<Ui::Checkbox>(
				box,
				TypeLabel(type),
				bool(state->types & type),
				st::defaultBoxCheckbox),
			st::boxRowPadding + style::margins(0, 0, 0, st::boxLittleSkip));
		checkbox->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			if (checked) {
				state->types |= type;
			} else {
				state->types &= ~MediaTypes(type);
			}
		}, checkbox->lifetime());
	}

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::ayu_ChatMediaDownloadPeriod());
	const auto addDate = [&](
			rpl::producer<QString> text,
			not_null<rpl::variable<TimeId>*> value,
			Fn<QString()> empty) {
		auto label = value->value() | rpl::map([=](TimeId date) {
			return date ? FormatDate(date) : empty();
		});
		const auto button = Settings::AddButtonWithLabel(
			container,
			std::move(text),
			std::move(label),
			st::settingsButtonNoIcon);
		button->setClickedCallback([=] {
			EditDate(controller, value->current(), [=](TimeId date) {
				*value = date;
			});
		});
	};
	addDate(tr::ayu_ChatMediaDownloadFrom(), &state->from, [] {
		return tr::ayu_ChatMediaDownloadBeginning(tr::now);
	});
	addDate(tr::ayu_ChatMediaDownloadTill(), &state->till, [] {
		return tr::ayu_ChatMediaDownloadEnd(tr::now);
	});

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, tr::ayu_ChatMediaDownloadFolder());
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			state->folder.value(),
			st::boxLabel),
		st::boxRowPadding);
	const auto change = Settings::AddButtonWithIcon(
		container,
		tr::ayu_ChatMediaDownloadChangeFolder(),
		st::settingsButtonNoIcon);
	change->setClickedCallback([=] {
		FileDialog::GetFolder(
			box.get(),
			tr::lng_download_path_choose(tr::now),
			state->folder.current(),
			crl::guard(box, [=](QString &&result) {
				if (!result.isEmpty()) {
					state->folder = QDir::cleanPath(result);
				}
			}));
	});

	Ui::AddSkip(container);
	Ui::AddDividerText(container, tr::ayu_ChatMediaDownloadAbout(
		tr::marked));

	const auto peerId = peer->id;
	box->addButton(tr::ayu_ChatMediaDownloadStart(), [=] {
		if (!state->types) {
			controller->showToast(tr::ayu_ChatMediaDownloadNoTypes(tr::now));
			return;
		}
		auto from = state->from.current();
		auto till = state->till.current();
		if (from && till && from > till) {
			std::swap(from, till);
		}
		const auto nextDay = [](TimeId date) {
			const auto day = base::unixtime::parse(date).date();
			return base::unixtime::serialize(day.addDays(1).startOfDay());
		};
		const auto migrated = topicRootId ? nullptr : peer->migrateFrom();
		auto settings = JobSettings{
			.peerId = peerId,
			.migratedPeerId = migrated ? migrated->id : PeerId(),
			.topicRootId = topicRootId,
			.folder = state->folder.current(),
			.types = state->types,
			.minDate = from ? (from - 1) : 0,
			.maxDate = std::min(
				till ? nextDay(till) : std::numeric_limits<TimeId>::max(),
				base::unixtime::now() + 1),
		};
		const auto manager = Manager::Instance(&controller->session());
		manager->create(std::move(settings))->start();
		box->closeBox();
		controller->show(Box(
			FillProgressBox,
			controller,
			peerId,
			topicRootId));
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

void ShowChatMediaDownload(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId topicRootId) {
	const auto manager = Manager::Instance(&controller->session());
	if (manager->find(peer->id, topicRootId)) {
		controller->show(Box(
			FillProgressBox,
			controller,
			peer->id,
			topicRootId));
	} else {
		controller->show(Box(FillSetupBox, controller, peer, topicRootId));
	}
}

} // namespace AyuUi
