// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_MAINWINDOW_H_
#define MUMBLE_MUMBLE_MAINWINDOW_H_

#include <QtCore/QPointer>
#include <QtCore/QtGlobal>
#include <QtNetwork/QAbstractSocket>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QSizeGrip>

#include "AudioOutputToken.h"
#include "CustomElements.h"
#include "Log.h"
#include "MUComboBox.h"
#include "Mumble.pb.h"
#include "MumbleProtocol.h"
#include "QtUtils.h"
#include "Usage.h"
#include "UserLocalNicknameDialog.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stack>
#include <unordered_map>

#include "ui_MainWindow.h"

#define MB_QEVENT (QEvent::User + 939)
#define OU_QEVENT (QEvent::User + 940)

class ACLEditor;
class BanEditor;
class UserEdit;
class ServerHandler;
class GlobalShortcut;
class TextToSpeech;
class UserModel;
class Tokens;
class Channel;
class UserInformation;
class VoiceRecorderDialog;
class PositionalAudioViewer;
class PTTButtonWidget;

namespace Search {
class SearchDialog;
}

class MenuLabel;
class ListenerVolumeSlider;
class UserLocalVolumeSlider;

struct ShortcutTarget;

struct ContextMenuTarget {
	ClientUser *user = nullptr;
	Channel *channel = nullptr;
};

class MessageBoxEvent : public QEvent {
public:
	QString msg;
	MessageBoxEvent(QString msg);
};

class OpenURLEvent : public QEvent {
public:
	QUrl url;
	OpenURLEvent(QUrl url);
};


class AudioOutputScreenShare;
class ScreenAudioBroadcaster;
class VideoBroadcaster;
class VideoGrid;
class VideoStreamDispatcher;

class MainWindow : public QMainWindow, public Ui::MainWindow {
	friend class UserModel;

private:
	Q_OBJECT
	Q_DISABLE_COPY(MainWindow)
public:
	UserModel *pmModel;
	QMenu *qmUser;
	QMenu *qmChannel;
	QMenu *qmListener;
	QMenu *qmDeveloper;
	QIcon qiIcon, qiIconMutePushToMute, qiIconMuteSelf, qiIconMuteServer, qiIconDeafSelf, qiIconDeafServer,
		qiIconMuteSuppressed;
	QIcon qiTalkingOn, qiTalkingWhisper, qiTalkingShout, qiTalkingOff;
	QIcon m_iconInformation;
	std::unordered_map< unsigned int, qt_unique_ptr< UserLocalNicknameDialog > > qmUserNicknameTracker;

	/// "Action" for when there are no actions available
	QAction *qaEmpty;

	GlobalShortcut *gsPushTalk, *gsResetAudio, *gsMuteSelf, *gsDeafSelf;
	GlobalShortcut *gsUnlink, *gsPushMute, *gsJoinChannel;
#ifdef USE_OVERLAY
	GlobalShortcut *gsToggleOverlay;
#endif
	GlobalShortcut *gsMinimal, *gsVolumeUp, *gsVolumeDown, *gsWhisper, *gsLinkChannel, *gsListenChannel;
	GlobalShortcut *gsCycleTransmitMode, *gsToggleMainWindowVisibility, *gsTransmitModePushToTalk,
		*gsTransmitModeContinuous, *gsTransmitModeVAD;
	GlobalShortcut *gsSendTextMessage, *gsSendClipboardTextMessage;
	GlobalShortcut *gsToggleTalkingUI;
	GlobalShortcut *gsToggleSearch;
	GlobalShortcut *gsServerConnect, *gsServerDisconnect, *gsServerInformation, *gsServerTokens;
	GlobalShortcut *gsServerUserList, *gsServerBanList;
	GlobalShortcut *gsSelfPrioritySpeaker;
	GlobalShortcut *gsRecording;
	GlobalShortcut *gsSelfComment, *gsServerTexture, *gsServerTextureRemove;
	GlobalShortcut *gsSelfRegister, *gsAudioStats;
	GlobalShortcut *gsConfigDialog, *gsAudioWizard, *gsConfigCert;
	GlobalShortcut *gsAudioTTS;
	GlobalShortcut *gsHelpAbout, *gsHelpAboutQt, *gsHelpVersionCheck;
	GlobalShortcut *gsTogglePositionalAudio;
	GlobalShortcut *gsMoveBack;
	GlobalShortcut *gsCycleListenerAttenuationMode, *gsListenerAttenuationUp, *gsListenerAttenuationDown;
	GlobalShortcut *gsAdaptivePush;

	DockTitleBar *dtbLogDockTitle, *dtbChatDockTitle;

	ACLEditor *aclEdit;
	BanEditor *banEdit;
	UserEdit *userEdit;
	Tokens *tokenEdit;

	VoiceRecorderDialog *voiceRecorderDialog;

	MumbleProto::Reject_RejectType rtLast;
	bool bRetryServer;
	QString qsDesiredChannel;

	bool forceQuit;
	/// Restart the client after shutdown
	bool restartOnQuit;

	/// Contains the cursor whose position is immediately before the image to
	/// save when activating the "Save Image As..." context menu item.
	QTextCursor qtcSaveImageCursor;

	QPointer< Channel > cContextChannel;
	QPointer< ClientUser > cuContextUser;

	QPoint qpContextPosition;

	void recheckTTS();
	void msgBox(QString msg);
	void setOnTop(bool top);
	void setShowDockTitleBars(bool doShow);
	void updateAudioToolTips();
	void updateUserModel();
	void focusNextMainWidget();
	QPair< QByteArray, QImage > openImageFile();

	void loadState(bool minimalView);
	void storeState(bool minimalView);

	void updateChatBar();
	void openTextMessageDialog(ClientUser *p);
	void openUserLocalNicknameDialog(const ClientUser &p);

#ifdef Q_OS_WIN
	bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) Q_DECL_OVERRIDE;
	unsigned int uiNewHardware;
#endif
protected:
	Usage uUsage;
	QTimer *qtReconnect;
	std::unique_ptr< NotificationSoundBlocker > m_reconnectSoundBlocker;

	QList< QAction * > qlServerActions;
	QList< QAction * > qlChannelActions;
	QList< QAction * > qlUserActions;

	QHash< ShortcutTarget, int > qmCurrentTargets;
	/// A map that contains information about the currently active
	/// shout/whisper targets. The mapping is between a List of
	/// ShortcutTargets that are all triggered together and the
	/// target ID for this specific combination of ShortcutTargets.
	/// The target ID is what the server uses to identify this specific
	/// set of ShortcutTargets.
	QHash< QList< ShortcutTarget >, int > qmTargets;
	/// This is a map between all target IDs the client will ever use
	/// and a helper-number (see iTargetCounter).
	QMap< int, int > qmTargetUse;
	Channel *mapChannel(int idx) const;
	/// This is a pure helper number whose job is to always be increased
	/// if a new VoiceTarget is needed. It will be used as the helper
	/// number in qmTargetUse.
	int iTargetCounter;
	QMap< unsigned int, UserInformation * > qmUserInformations;

	std::unique_ptr< PositionalAudioViewer > m_paViewer;

	PTTButtonWidget *qwPTTButtonWidget;

	MUComboBox *qcbTransmitMode;
	QAction *qaTransmitMode;
	QAction *qaTransmitModeSeparator;

	Search::SearchDialog *m_searchDialog = nullptr;

	qt_unique_ptr< MenuLabel > m_localVolumeLabel;
	qt_unique_ptr< UserLocalVolumeSlider > m_userLocalVolumeSlider;
	qt_unique_ptr< ListenerVolumeSlider > m_listenerVolumeSlider;

	std::stack< unsigned int > m_previousChannels;
	std::optional< unsigned int > m_movedBackFromChannel;

	static constexpr int stateVersion();

	/// Snapshot of the dock/toolbar layout - saveState()'s output, not saveGeometry()'s - taken the
	/// instant the window starts minimizing and reapplied the instant it finishes restoring. On Windows,
	/// QMainWindow's dock area layout has a known habit of recomputing splitter proportions by size hint
	/// across a minimize/restore round trip rather than preserving the exact pixel split a user last
	/// dragged a splitter to; this puts it back afterward rather than leaving whatever that round trip
	/// produced. See changeEvent()'s WindowStateChange handling.
	QByteArray m_preMinimizeDockState;

	void createActions();
	void setupGui();

	/// Shows the video other participants are sharing. Created in setupGui and hidden until somebody
	/// actually shares something, so the window looks unchanged for users who never use the feature.
	VideoGrid *m_videoGrid   = nullptr;
	QDockWidget *m_videoDock = nullptr;

	/// Whether the video dock is currently shown, so its auto-sizing fires only on the hidden<->shown
	/// transition and not on every join, leave or toggle within an ongoing call.
	bool m_videoDockShown = false;

	/// Pulls OpusAudio-coded units off the same signal m_videoGrid listens to, for screen-share audio.
	VideoStreamDispatcher *m_videoStreamDispatcher = nullptr;

	/// Creates the video dock and connects it to the network.
	void setupVideoGrid();

	/// Sends and receives the local camera stream.
	VideoBroadcaster *m_videoBroadcaster = nullptr;
	QAction *m_shareCameraAction         = nullptr;

	/// Creates the share-camera action and its wiring.
	void setupVideoBroadcast();

	/// Hands out non-colliding starting stream ids across every broadcaster this session runs at once
	/// (camera, screen video, screen audio). See VideoBroadcaster::setNextStreamID for why this has to be
	/// centralised rather than left to each broadcaster to pick its own.
	std::uint32_t m_nextVideoStreamID = 0;
	std::uint32_t allocateStreamID() { return m_nextVideoStreamID++; }

	QAction *m_videoWizardAction = nullptr;

	/// Sends the local screen share's picture and, if the user chose to include it, its audio. Two
	/// separate broadcasters rather than one: a picture and an audio stream are unrelated media as far as
	/// VideoBroadcaster's own machinery is concerned, and ScreenAudioBroadcaster's encode path (Opus, not
	/// a picture codec) has nothing in common with VideoBroadcaster's beyond the wire envelope they both
	/// happen to emit.
	VideoBroadcaster *m_screenVideoBroadcaster       = nullptr;
	ScreenAudioBroadcaster *m_screenAudioBroadcaster = nullptr;
	QAction *m_shareScreenAction                     = nullptr;

	/// Creates the share-screen action and its wiring.
	void setupScreenShare();

	/// Received screen-share audio, one entry per (sender, stream) currently playing. `buffer` is a
	/// non-owning pointer used only to call addOpusPacket() - actual ownership belongs to AudioOutput
	/// from the moment addExternalBuffer() hands it over, which is also why removal has to go through
	/// `token` (AudioOutput::invalidateToken), not a raw delete. Keyed the same way VideoGrid keys its
	/// own surfaces.
	struct ScreenShareAudioEntry {
		AudioOutputToken token;
		AudioOutputScreenShare *buffer = nullptr;
	};

	std::unordered_map< std::uint64_t, ScreenShareAudioEntry > m_screenShareAudioBuffers;

	/// No-op if (senderSession, streamID) is not currently a screen-share audio stream - callers do not
	/// need to know whether a given ending stream was one before asking this to check.
	void removeScreenShareAudioBuffer(unsigned int senderSession, unsigned int streamID);

	/// Removes every screen-share audio buffer belonging to a sender, whatever its stream id - used when
	/// the sender disconnects and no per-stream end message will arrive.
	void removeScreenShareAudioBuffersForSender(unsigned int senderSession);

	/// Applies a per-tile volume slider's value to whichever screen-share audio buffer(s) belong to this
	/// sender. Sender-keyed rather than stream-keyed, same reasoning as removeScreenShareAudioBuffersForSender.
	void setScreenShareVolumeForSender(unsigned int senderSession, float multiplier);

	static std::uint64_t screenShareAudioKey(unsigned int senderSession, unsigned int streamID) {
		return (static_cast< std::uint64_t >(senderSession) << 32) | streamID;
	}

	/// When a keyframe was last requested from each (sender, stream), so a stuck decoder asks at most
	/// once per interval rather than on every failing unit. Keyed like m_screenShareAudioBuffers.
	std::unordered_map< std::uint64_t, qint64 > m_lastKeyframeRequestMsec;
	/// A resize handle in the bottom-right corner. Wayland compositors draw no server-side frame for
	/// Qt windows (GNOME never does), leaving only Qt's own thin client-side border to grab, which in
	/// practice means the window cannot be resized by dragging at all. QSizeGrip asks the compositor
	/// for an interactive resize, which works everywhere.
	QSizeGrip *m_sizeGrip = nullptr;

	static constexpr qint64 KEYFRAME_REQUEST_INTERVAL_MSEC = 1000;

public slots:
	/// Opens the guided video setup.
	void openVideoWizardDialog();

protected:
public slots:
	/// Starts or stops sharing the local camera, and tells the server either way.
	void toggleCameraShare(bool share);

	/// Starts or stops sharing the local screen (and, if chosen, its audio), and tells the server either
	/// way. Opens ScreenSharePickerDialog to ask what to share when starting; unlike the camera there is
	/// no sensible default to fall back to.
	void toggleScreenShare(bool share);

	/// One Opus packet belonging to some sender's screen-share audio, forwarded from
	/// VideoStreamDispatcher::opusUnitReceived. Finds or creates that (sender, stream)'s
	/// AudioOutputScreenShare and feeds it.
	void onScreenShareOpusUnitReceived(unsigned int senderSession, unsigned int streamID, const QByteArray &opusPacket);

	void updateWindowTitle();
	/// updateToolbar updates the state of the toolbar depending on the current
	/// window layout setting.
	/// If the window layout setting is 'custom', the toolbar is made movable. If the
	/// window layout is not 'custom', the toolbar is locked in place at the top of
	/// the MainWindow.
	void updateToolbar();
	void updateFavoriteButton();
	void customEvent(QEvent *evt) Q_DECL_OVERRIDE;
	void findDesiredChannel();
	void setupView(bool toggle_minimize = true);
	void closeEvent(QCloseEvent *e) Q_DECL_OVERRIDE;
	void resizeEvent(QResizeEvent *e) Q_DECL_OVERRIDE;

	void hideEvent(QHideEvent *e) Q_DECL_OVERRIDE;
	void showEvent(QShowEvent *e) Q_DECL_OVERRIDE;
	void changeEvent(QEvent *e) Q_DECL_OVERRIDE;
	void keyPressEvent(QKeyEvent *e) Q_DECL_OVERRIDE;

	QMenu *createPopupMenu() Q_DECL_OVERRIDE;

	bool handleSpecialContextMenu(const QUrl &url, const QPoint &pos_, bool focus = false);
	Channel *getContextMenuChannel();
	ClientUser *getContextMenuUser();
	ContextMenuTarget getContextMenuTargets();

	void autocompleteUsername();

public slots:
	void on_qmServer_aboutToShow();
	void on_qaServerConnect_triggered(bool autoconnect = false);
	void on_qaServerDisconnect_triggered();
	void on_qaServerBanList_triggered();
	void on_qaServerUserList_triggered();
	void on_qaServerInformation_triggered();
	void on_qaServerTexture_triggered();
	void on_qaServerTextureRemove_triggered();
	void on_qaServerTokens_triggered();
	void on_qmSelf_aboutToShow();
	void on_qaSelfComment_triggered();
	void on_qaSelfRegister_triggered();
	void qcbTransmitMode_activated(int index);
	void updateTransmitModeComboBox(Settings::AudioTransmit newMode);
	void qmUser_aboutToShow();
	void qmListener_aboutToShow();
	void on_qaUserCommentReset_triggered();
	void on_qaUserTextureReset_triggered();
	void on_qaUserCommentView_triggered();
	void on_qaUserKick_triggered();
	void on_qaUserBan_triggered();
	void on_qaUserMute_triggered();
	void on_qaUserDeaf_triggered();
	void on_qaSelfPrioritySpeaker_triggered();
	void on_qaUserPrioritySpeaker_triggered();
	void on_qaUserLocalIgnore_triggered();
	void on_qaUserLocalIgnoreTTS_triggered();
	void on_qaUserLocalMute_triggered();
	void on_qaUserLocalNickname_triggered();
	void on_qaUserTextMessage_triggered();
	void on_qaUserRegister_triggered();
	void on_qaUserInformation_triggered();
	void on_qaUserFriendAdd_triggered();
	void on_qaUserFriendRemove_triggered();
	void on_qaUserFriendUpdate_triggered();
	void qmChannel_aboutToShow();
	void on_qaChannelJoin_triggered();
	void on_qaUserJoin_triggered();
	void on_qaUserMove_triggered();
	void on_qaChannelListen_triggered();
	void on_qaChannelAdd_triggered();
	void on_qaChannelRemove_triggered();
	void on_qaChannelACL_triggered();
	void on_qaChannelLink_triggered();
	void on_qaChannelUnlink_triggered();
	void on_qaChannelUnlinkAll_triggered();
	void on_qaChannelSendMessage_triggered();
	void on_qaChannelHide_triggered();
	void on_qaChannelPin_triggered();
	void on_qaChannelCopyURL_triggered();
	void on_qaAudioReset_triggered();
	void on_qaAudioMute_triggered();
	void on_qaAudioDeaf_triggered();
	void on_qaRecording_triggered();
	void on_qaAudioTTS_triggered();
	void on_qaAudioUnlink_triggered();
	void on_qaAudioStats_triggered();
	void on_qaConfigDialog_triggered();
	void on_qaConfigHideFrame_triggered();
	void on_qmConfig_aboutToShow();
	void on_qaConfigMinimal_triggered();
	void on_qaConfigCert_triggered();
	void on_qaAudioWizard_triggered();
	void on_qaDeveloperConsole_triggered();
	void on_qaPositionalAudioViewer_triggered();
	void on_qaHelpWhatsThis_triggered();
	void on_qaHelpAbout_triggered();
	void on_qaHelpAboutQt_triggered();
	void on_qaHelpVersionCheck_triggered();
	void on_qaQuit_triggered();
	void on_qteChat_tabPressed();
	void on_qteChat_backtabPressed();
	void on_qteChat_ctrlSpacePressed();
	void on_qtvUsers_customContextMenuRequested(const QPoint &mpos, bool usePositionForGettingContext = true);
	void on_qteLog_customContextMenuRequested(const QPoint &pos);
	void on_qteLog_anchorClicked(const QUrl &);
	void on_qteLog_highlighted(const QUrl &link);
	void on_PushToTalk_triggered(bool, QVariant);
	void on_PushToMute_triggered(bool, QVariant);
	void on_VolumeUp_triggered(bool, QVariant);
	void on_VolumeDown_triggered(bool, QVariant);
	void on_gsMuteSelf_down(QVariant);
	void on_gsDeafSelf_down(QVariant);
	void on_gsWhisper_triggered(bool, QVariant);
	void addTarget(ShortcutTarget *);
	void removeTarget(ShortcutTarget *);
	void on_gsListenChannel_triggered(bool, QVariant);
	void on_gsCycleTransmitMode_triggered(bool, QVariant);
	void on_gsToggleMainWindowVisibility_triggered(bool, QVariant);
	void on_gsTransmitModePushToTalk_triggered(bool, QVariant);
	void on_gsTransmitModeContinuous_triggered(bool, QVariant);
	void on_gsTransmitModeVAD_triggered(bool, QVariant);
	void on_gsSendTextMessage_triggered(bool, QVariant);
	void on_gsSendClipboardTextMessage_triggered(bool, QVariant);
	void on_gsToggleTalkingUI_triggered(bool, QVariant);
	void on_gsToggleSearch_triggered(bool, QVariant);
	void on_gsServerConnect_triggered(bool, QVariant);
	void on_gsServerDisconnect_triggered(bool, QVariant);
	void on_gsServerInformation_triggered(bool, QVariant);
	void on_gsServerTokens_triggered(bool, QVariant);
	void on_gsServerUserList_triggered(bool, QVariant);
	void on_gsServerBanList_triggered(bool, QVariant);
	void on_gsSelfPrioritySpeaker_triggered(bool, QVariant);
	void on_gsRecording_triggered(bool, QVariant);
	void on_gsSelfComment_triggered(bool, QVariant);
	void on_gsServerTexture_triggered(bool, QVariant);
	void on_gsServerTextureRemove_triggered(bool, QVariant);
	void on_gsSelfRegister_triggered(bool, QVariant);
	void on_gsAudioStats_triggered(bool, QVariant);
	void on_gsConfigDialog_triggered(bool, QVariant);
	void on_gsAudioWizard_triggered(bool, QVariant);
	void on_gsConfigCert_triggered(bool, QVariant);
	void on_gsAudioTTS_triggered(bool, QVariant);
	void on_gsHelpAbout_triggered(bool, QVariant);
	void on_gsHelpAboutQt_triggered(bool, QVariant);
	void on_gsHelpVersionCheck_triggered(bool, QVariant);
	void on_gsTogglePositionalAudio_triggered(bool, QVariant);
	void on_gsMoveBack_triggered(bool, QVariant);
	void on_gsCycleListenerAttenuationMode_triggered(bool, QVariant);
	void on_gsListenerAttenuationUp_triggered(bool, QVariant);
	void on_gsListenerAttenuationDown_triggered(bool, QVariant);
	void on_gsAdaptivePush_triggered(bool, QVariant);

	void on_Reconnect_timeout();
	void on_qaTalkingUIToggle_triggered();
	void voiceRecorderDialog_finished(int);
	void qtvUserCurrentChanged(const QModelIndex &, const QModelIndex &);
	void serverConnected();
	void serverDisconnected(QAbstractSocket::SocketError, QString reason);
	void resolverError(QAbstractSocket::SocketError, QString reason);
	void viewCertificate(bool);
	void openUrl(const QUrl &url);
	void context_triggered();
	void updateTarget();
	void updateMenuPermissions();
	void on_muteCuePopup_triggered();
	/// Handles state changes like talking mode changes and mute/unmute
	/// or priority speaker flag changes for the gui user
	void userStateChanged();
	void on_channelStateChanged(Channel *channel, bool forceUpdateTree);
	void destroyUserInformation();
	void sendChatbarMessage(QString msg);
	void sendChatbarText(QString msg, bool plainText = false);
	void pttReleased();
	void whisperReleased(QVariant scdata);
	void onResetAudio();
	void showRaiseWindow();
	void on_qaFilterToggle_triggered();
	/// Opens a save dialog for the image referenced by qtcSaveImageCursor.
	void saveImageAs();
	/// Returns the path to the user's image directory, optionally with a
	/// filename included.
	QString getImagePath(QString filename = QString()) const;

	/// Shows a dialog with the image from the context menu as a QPixmap
	void showImageDialog();
	/// Updates the user's image directory to the given path (any included
	/// filename is discarded).
	void updateImagePath(QString filepath) const;
	void setTransmissionMode(Settings::AudioTransmit mode);
	/// Sets the local user's mute state
	///
	/// @param mute Whether to mute the user
	void setAudioMute(bool mute);
	/// Sets the local user's deaf state
	///
	/// @param deaf Whether to deafen the user
	void setAudioDeaf(bool deaf);
	// Callback the search action being triggered
	void on_qaSearch_triggered();
	void toggleSearchDialogVisibility();
	/// Enables or disables the recording feature
	void enableRecording(bool recordingAllowed);
	/// Invokes OS native window highlighting
	void highlightWindow();
	void on_user_moved(unsigned int sessionID, const std::optional< unsigned int > &prevChannelID,
					   unsigned int newChannelID);
	void on_qaMoveBack_triggered();
signals:
	/// Signal emitted when the server and the client have finished
	/// synchronizing (after a new connection).
	void serverSynchronized();
	/// Signal emitted whenever a user adds a new ChannelListener
	void userAddedChannelListener(ClientUser *user, Channel *channel);
	/// Signal emitted whenever a user removes a ChannelListener
	void userRemovedChannelListener(ClientUser *user, Channel *channel);
	void transmissionModeChanged(Settings::AudioTransmit newMode);

	/// Signal emitted when the local user changes their talking status either actively or passively
	void talkingStatusChanged();

	/// Signal when channel state has been changed
	void channelStateChanged(Channel *channel, bool forceUpdateTree);

	/// Signal emitted when the connection was terminated and all cleanup code has been run
	void disconnectedFromServer();

	/// Signal emitted when the window manager notifies the Mumble MainWindow that the application was just minimized
	void windowMinimized();
	/// Signal emitted when the user requested to toggle the MainWindow visibility
	void windowVisibilityToggled();
	/// Signal emitted whenever the Mumble MainWindow regains the active state from the window manager
	void windowActivated();

public:
	MainWindow(QWidget *parent);
	~MainWindow() Q_DECL_OVERRIDE;

	// Implementation in Messages.cpp
#define PROCESS_MUMBLE_TCP_MESSAGE(name, value) void msg##name(const MumbleProto::name &);
	MUMBLE_ALL_TCP_MESSAGES
#undef PROCESS_MUMBLE_TCP_MESSAGE
	void removeContextAction(const MumbleProto::ContextActionModify &msg);
	/// Logs a message that an action could not be saved permanently because
	/// the user has no certificate and can't be reliably identified.
	///
	/// @param actionName  The name of the action that has been executed.
	/// @param p  The user on which the action was performed.
	void logChangeNotPermanent(const QString &actionName, ClientUser *const p) const;

	void openServerConnectDialog(bool autoconnect = false);
	void disconnectFromServer();
	void addServerAsFavorite();
	void openServerInformationDialog();
	void openServerTokensDialog();
	void openServerUserListDialog();
	void openServerBanListDialog();
	void toggleSelfPrioritySpeaker();
	void recording();
	void openSelfCommentDialog();
	void changeServerTexture();
	void removeServerTexture();
	void selfRegister();
	void openAudioStatsDialog();
	void openConfigDialog();
	void openAudioWizardDialog();
	void openCertWizardDialog();
	void enableAudioTTS(bool enable);
	void openAboutDialog();
	void openAboutQtDialog();
	void versionCheck();
	void enablePositionalAudio(bool enable);
};

#endif
