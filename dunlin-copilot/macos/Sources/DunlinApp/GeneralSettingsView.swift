import AppKit
import SwiftUI
import UserNotifications

private struct ChatBodySizeKey: EnvironmentKey { static let defaultValue: CGFloat = 18 }
extension EnvironmentValues {
  var chatBodySize: CGFloat {
    get { self[ChatBodySizeKey.self] }
    set { self[ChatBodySizeKey.self] = newValue }
  }
}
struct GeneralSettingsView: View {
  @EnvironmentObject var store: WorkspaceStore
  var body: some View {
    VStack(alignment: .leading, spacing: 42) {
      settingsGroup("外观与显示") {
        settingsRow("外观", detail: "科技白与科技黑随应用内置。") { ThemePicker() }
        Divider()
        settingsRow("文字大小") {
          Picker("文字大小", selection: $store.generalPreferences.textSize) {
            ForEach(ChatTextSize.allCases, id: \.self) { Text($0.title).tag($0) }
          }.labelsHidden().fixedSize().accessibilityIdentifier("settings.text-size")
        }
        Divider()
        settingsRow("语言") { Text("简体中文").foregroundStyle(.secondary) }
      }
      settingsGroup("输入与通知") {
        settingsRow("发送消息", detail: "Shift + Return 换行；输入法候选确认不会发送。") {
          Picker("发送消息", selection: $store.generalPreferences.sendKey) {
            ForEach(ChatSendKey.allCases, id: \.self) { Text($0.title).tag($0) }
          }.labelsHidden().fixedSize().accessibilityIdentifier("settings.send-key")
        }
        Divider()
        settingsRow("完成时通知", detail: "应用不在前台时提醒。") {
          Toggle(
            "完成时通知",
            isOn: Binding(
              get: { store.generalPreferences.notifyOnCompletion },
              set: { store.setCompletionNotifications($0) })
          )
          .labelsHidden().toggleStyle(.switch).disabled(store.notificationPending)
          .accessibilityIdentifier("settings.completion-notice")
        }
        if !store.notificationStatus.isEmpty {
          Text(store.notificationStatus).font(.caption).foregroundStyle(.secondary).frame(maxWidth:.infinity,alignment:.leading).padding(.bottom,12)
        }
        Divider()
        settingsRow("显示公开推理摘要", detail: "仅显示服务商返回的可展示内容。") {
          Toggle("显示公开推理摘要", isOn: $store.showReasoning).labelsHidden().toggleStyle(.switch)
            .accessibilityIdentifier("settings.public-reasoning")
        }
      }
      Text("设置更改自动保存。系统透明度、对比度与动态效果设置会自动遵循。")
        .font(.system(size:13)).foregroundStyle(.secondary).padding(.horizontal,20).padding(.top,16)
    }.onAppear { store.refreshNotificationStatus() }
  }
  private func settingsGroup<Content: View>(_ title: String, @ViewBuilder content: () -> Content)
    -> some View
  {
    VStack(alignment: .leading, spacing: 18) {
      Text(title).font(.system(size: 16, weight: .semibold)).padding(.horizontal,20)
      VStack(spacing:0) {content()}.padding(.horizontal,20).background(store.themeSurface)
    }
  }
  private func settingsRow<Control: View>(
    _ title: String, detail: String? = nil, @ViewBuilder control: () -> Control
  ) -> some View {
    HStack(spacing: 24) {
      VStack(alignment: .leading, spacing: 6) {
        Text(title).font(.system(size: 14, weight: .medium))
        if let detail { Text(detail).font(.system(size: 12)).foregroundStyle(.secondary) }
      }
      Spacer(minLength: 10)
      control()
    }.frame(minHeight:64)
  }
}
struct ThemePicker: View {
  @EnvironmentObject var store: WorkspaceStore
  var body: some View {
    Picker(
      "外观",
      selection: Binding(
        get: {
          ["system", "light", "dark"].contains(store.theme)
            || store.installedThemes.contains(where: { $0.id == store.theme })
            ? store.theme : "system"
        }, set: { store.theme = $0 })
    ) {
      Text("跟随系统").tag("system")
      Text("科技白").tag("light")
      Text("科技黑").tag("dark")
      ForEach(store.installedThemes) { Text($0.name).tag($0.id) }
    }.labelsHidden().fixedSize().accessibilityIdentifier("settings.theme")
      .onAppear { store.refreshThemes() }
  }
}
extension WorkspaceStore {
  func refreshNotificationStatus() {
    let epoch = identityEpoch
    UNUserNotificationCenter.current().getNotificationSettings { settings in
      let status = settings.authorizationStatus
      DispatchQueue.main.async {
        guard self.identityEpoch == epoch else { return }
        switch status {
        case .denied: self.notificationStatus = "系统通知已关闭，可在系统设置的“通知”中开启 Dunlin Copilot。"
        case .notDetermined: self.notificationStatus = "首次开启时，系统会询问通知权限。"
        default: self.notificationStatus = ""
        }
      }
    }
  }
  func setCompletionNotifications(_ enabled: Bool) {
    notificationRequestID = UUID()
    guard enabled else {
      generalPreferences.notifyOnCompletion = false
      notificationPending = false
      saveSettings()
      return
    }
    let request = notificationRequestID
    let epoch = identityEpoch
    notificationPending = true
    UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) {
      allowed, error in
      let failed = error != nil
      DispatchQueue.main.async {
        guard self.identityEpoch == epoch, self.notificationRequestID == request else { return }
        self.notificationPending = false
        self.generalPreferences.notifyOnCompletion = allowed && !failed
        self.notificationStatus =
          failed
          ? "无法请求系统通知权限，请稍后重试。"
          : allowed ? "" : "系统未允许通知，可在系统设置的“通知”中开启 Dunlin Copilot。"
        self.saveSettings()
      }
    }
  }
  func notifyCompletion(run: UUID, succeeded: Bool) {
    guard succeeded, generalPreferences.notifyOnCompletion, !NSApp.isActive else { return }
    let epoch = identityEpoch
    UNUserNotificationCenter.current().getNotificationSettings { settings in
      guard
        settings.authorizationStatus == .authorized || settings.authorizationStatus == .provisional
      else { return }
      DispatchQueue.main.async {
        guard self.identityEpoch == epoch, self.generalPreferences.notifyOnCompletion,
          !NSApp.isActive
        else { return }
        let content = UNMutableNotificationContent()
        content.title = "Dunlin Copilot"
        content.body = "本次回复已完成。"
        content.sound = .default
        UNUserNotificationCenter.current().add(
          UNNotificationRequest(
            identifier: "reply." + run.uuidString, content: content, trigger: nil))
      }
    }
  }
}
