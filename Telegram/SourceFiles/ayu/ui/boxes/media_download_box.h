// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

class PeerData;

namespace Window {
class SessionController;
} // namespace Window

namespace AyuUi {

void ShowChatMediaDownload(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	MsgId topicRootId);

} // namespace AyuUi
