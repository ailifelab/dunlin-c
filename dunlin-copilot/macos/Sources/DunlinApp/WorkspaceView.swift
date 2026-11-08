import AppKit
import SwiftUI

struct MainView: View {
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.openSettings) private var openSettings
  @State private var grantsVisible = false
  @State private var recoveryVisible = false
  @State private var fileHistoryVisible = false
  var body: some View {
    NavigationSplitView {
      VStack(alignment: .leading, spacing: 0) {
        Text("Dunlin Copilot").font(.system(size: 20, weight: .semibold)).padding(.horizontal, 24)
          .padding(.top, 22).padding(.bottom, 18)
        List(selection: $store.selected) {
          Section {
            Button {
              store.newChat()
              store.searchVisible = false
            } label: {
              Label("新聊天", systemImage: "plus").font(.system(size: 14, weight: .semibold))
                .imageScale(.large).frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
            }.buttonStyle(.plain).foregroundStyle(.primary).accessibilityIdentifier("chat.new")
            Button {
              store.searchVisible.toggle()
            } label: {
              Label("搜索", systemImage: "magnifyingglass").font(.system(size: 14)).imageScale(.large)
                .frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
            }.buttonStyle(.plain).foregroundStyle(.primary).accessibilityIdentifier("chat.search")
            Button(action: store.browseProjects) {
              Label("项目", systemImage: "folder").font(.system(size: 14)).imageScale(.large)
                .frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
            }
            .buttonStyle(.plain).foregroundStyle(.primary).accessibilityIdentifier("project.list")
            Button { store.route = "agents"; store.reloadAgentJobs() } label: {
              Label("Agent 协作", systemImage: "person.3").frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
            }.buttonStyle(.plain).accessibilityIdentifier("agents.open")
            Button(action: store.openToolRecords) {
              Label("工具记录", systemImage: "wrench.and.screwdriver")
                .frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
            }.buttonStyle(.plain).accessibilityIdentifier("tools.sidebar")
          }
          if !store.pluginPages.filter({$0.contribution.kind == "sidebar"}).isEmpty {
            Section("插件") {ForEach(store.pluginPages.filter {$0.contribution.kind == "sidebar"}) {page in
              Button {store.pluginPageID = page.id;store.route = "plugin"} label: {Label(page.contribution.title ?? page.package,systemImage:"puzzlepiece.extension")}
            }}
          }
          if store.chats.contains(where: { $0.pinned && !$0.archived }) {
            Section("置顶") {
              ForEach(store.chats.filter { $0.pinned && !$0.archived }) { chatRow($0) }
            }
          }
          if !store.projects.isEmpty {
            Section {
              ForEach(store.projects) { project in
                ProjectSidebarHeader(project: project,
                  expand: { store.setExpanded(project, !project.expanded) },
                  properties: { store.editProject(project) },
                  open: { store.openProject(project) })
                  .frame(height: 25).contextMenu {
                    Button("项目主页") { store.openProject(project) }
                    Button("项目属性…") { store.editProject(project) }
                    Button("在访达中显示") { NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: project.path) }
                    Button("移除项目…", role: .destructive) { store.removeProject(project) }
                  }
                if project.expanded {
                  Button { store.newChat(workspace: project.path) } label: {
                    Text("新聊天").frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
                  }.padding(.leading, 18).disabled(!project.available)
                    .accessibilityIdentifier("project.new.\(project.id)")
                  ForEach(store.chats.filter { $0.project == project.id && !$0.archived }) { chat in
                    chatRow(chat).padding(.leading, 18)
                  }
                }
              }
            } header: {
              HStack {
                Text("项目")
                Spacer()
                Button(action: store.addProject) { Image(systemName: "plus") }
                  .help("新建项目").accessibilityIdentifier("project.add")
              }
            }
          }
          if store.chats.contains(where: { $0.project.isEmpty && !$0.archived && !$0.pinned }) {
            Section("最近聊天") {
              ForEach(store.chats.filter { $0.project.isEmpty && !$0.archived && !$0.pinned }) {
                chat in
                chatRow(chat)
              }
            }
          }
          if store.projects.isEmpty && !store.chats.contains(where: { !$0.archived }) {
            Text("项目和聊天会显示在这里").font(.caption).foregroundStyle(.secondary).padding(.top, 18)
          }
        }.listStyle(.sidebar).scrollContentBackground(.hidden).buttonStyle(.plain).font(
          .system(size: 13)
        ).disabled(store.busy || store.creatingChat || store.switchingModel || store.workspacePlanning || store.workspaceSaving || store.attachmentInteraction)
        Button {
          store.route = "archive"
        } label: {
          Label("已归档", systemImage: "archivebox").frame(maxWidth: .infinity, alignment: .leading)
        }.buttonStyle(.plain).foregroundStyle(.secondary).padding(.horizontal, 24).padding(
          .vertical, 10
        )
        .disabled(store.busy || store.creatingChat || store.switchingModel || store.workspacePlanning || store.workspaceSaving)
        .accessibilityIdentifier("chat.archive-list")
        SettingsLink {
          Label("设置…", systemImage: "gearshape").frame(maxWidth: .infinity, alignment: .leading)
        }.buttonStyle(.plain).padding(24).accessibilityIdentifier("settings.open")
      }.background {SidebarBackdrop().ignoresSafeArea()}.navigationSplitViewColumnWidth(min: 220, ideal: 246, max: 300)
    } detail: {
      HStack(spacing: 0) {
        VStack(spacing: 0) {
      if !store.pluginFaultNotice.isEmpty {
        HStack {
          Label(store.pluginFaultNotice,systemImage:"exclamationmark.triangle").font(.callout)
          Spacer()
          Button("查看日志") {store.settingsSection="插件";store.settingsPluginLogs=true;store.pluginFaultNotice="";openSettings()}
          Button {store.pluginFaultNotice=""} label:{Image(systemName:"xmark")}.accessibilityLabel("关闭插件提示")
        }.padding(12).background(.orange.opacity(0.1))
      }
          if store.route == "plugin" {
            PluginPageView(pageID:store.pluginPageID)
          } else if store.route == "agents" {
            AgentCollaborationView()
          } else if store.route == "tools" {
            ToolRecordsView()
          } else if store.route == "projects" {
            ProjectsView()
          } else if store.route == "project", let project = store.currentProject {
            ProjectHomeView(project: project)
          } else if store.route == "projectSettings", let project = store.editingProject {
            ProjectEditor(project: project, embedded: true).id(project.id)
          } else if store.route == "archive" {
            ArchivedChatsView()
          } else {
            if store.searchVisible { searchPane }
            if let project = store.currentProject, !project.available {
              HStack {
                Label(project.availability, systemImage: "exclamationmark.triangle")
                Spacer()
                Button("重新关联此聊天…", action: store.chooseChatWorkspace)
                  .disabled(store.selectedChat?.active == true || store.workspacePlanning || store.workspaceSaving)
                Button("项目属性…") { store.editProject(project) }
              }.padding().background(.orange.opacity(0.08))
            }
            if store.selected != nil {
              HStack {
                Picker("会话视图", selection: $store.conversationPage) {
                  Text("聊天").tag("chat")
                  Text("运行轨迹").tag("trajectory")
                  Text("通话记录").tag("calls")
                }.pickerStyle(.segmented).labelsHidden().frame(width: 320).accessibilityIdentifier(
                  "conversation.page")
                Spacer()
              }.padding(.horizontal, 28).padding(.vertical, 10).background(.bar)
            }
            if store.conversationPage == "calls", store.selected != nil {
              MediaHistoryView()
            } else if store.conversationPage == "tools", store.selected != nil {
              ToolRecordsView()
            } else if store.conversationPage == "trajectory", store.selected != nil {
              TrajectoryView(embedded: true)
            } else {
              if store.selected == nil && !store.chatCatalogLoaded {
                if store.error.isEmpty {
                  ProgressView("正在读取本地工作空间…").frame(maxWidth: .infinity, maxHeight: .infinity)
                } else {
                  ContentUnavailableView {
                    Label("工作空间暂不可读", systemImage: "externaldrive.badge.exclamationmark")
                  } description: {
                    Text("原始数据保留，请检查后重试。")
                  } actions: {
                    Button("重试", action: store.reload)
                  }
                }
              } else if store.profiles.isEmpty && store.selected == nil {
                FirstLaunchView()
              } else {
                if store.messagesLoading && store.messages.isEmpty {
                  ProgressView("正在读取聊天…").frame(maxWidth: .infinity, maxHeight: .infinity)
                } else if let error = store.messageReadError, store.messages.isEmpty {
                  ContentUnavailableView {
                    Label("无法读取聊天记录", systemImage: "exclamationmark.bubble")
                  } description: {
                    Text(error)
                  } actions: {
                    Button("重试", action: store.loadMessages)
                  }
                } else if store.messages.isEmpty && !store.busy {
                  NewConversationView()
                } else {
                  conversation
                }
                composer
              }
            }
          }
          if !store.error.isEmpty { errorBanner }
        }.frame(maxWidth: .infinity,maxHeight:.infinity)
        if store.inspectorVisible && store.isProject && store.route == "chat"
          && store.conversationPage == "chat"
        {
          Divider()
          inspector.frame(width: 282)
        }
      }.background(store.themeConversationBackground)
        .navigationTitle(
          store.route == "agents" ? "Agent 协作" : store.route == "tools" ? "工具执行记录" : store.route == "projects"
            ? "项目"
            : store.route == "archive"
              ? "已归档"
              : ["project", "projectSettings"].contains(store.route)
                ? (store.currentProject?.name ?? "项目") : (store.selectedChat?.title ?? "新聊天")
        )
        .toolbar {
          ToolbarItemGroup(placement: .primaryAction) {
            Menu {
              ForEach(store.identities.filter { !$0.hidden }) { identity in
                Button(identity.name + (identity.selected ? " ✓" : "")) {
                  store.switchIdentity(identity)
                }
                .disabled(identity.selected || store.busy)
              }
              Divider()
              Button("管理身份…") {
                store.settingsSection = "身份与家庭"
                openSettings()
              }
            } label: {
              Label(store.currentIdentity?.name ?? "本机身份", systemImage: "person.crop.circle")
            }
            .disabled(store.busy || store.creatingChat || store.switchingModel)
            .labelStyle(.titleAndIcon).fixedSize()
            .accessibilityLabel("当前身份：\(store.currentIdentity?.name ?? "本机身份")")
            .accessibilityIdentifier("identity.menu")
            Button {
              grantsVisible = true
            } label: {
              Label(
                "授权记录", systemImage: "lock.shield")
            }.accessibilityIdentifier("grants.open")
            Button {store.route="agents";store.reloadAgentJobs()} label: {Label("Agent 协作",systemImage:"person.3")}
              .accessibilityIdentifier("agents.toolbar")
          }
          ToolbarSpacer(.fixed, placement: .primaryAction)
          if store.selected != nil, store.route == "chat" {
            ToolbarItemGroup(placement: .primaryAction) {
              Button {
                store.conversationPage = "trajectory"
                store.trajectoryRunFocus = nil
                store.trajectoryCallFocus = nil
              } label: {
                Label("运行轨迹", systemImage: "point.3.connected.trianglepath.dotted")
              }.accessibilityIdentifier("trajectory.open")
              Button {
                store.openToolRecords()
              } label: {
                Label("工具记录", systemImage: "wrench.and.screwdriver")
              }.accessibilityIdentifier("tools.open")
            }
            ToolbarSpacer(.fixed, placement: .primaryAction)
          }
          ToolbarItemGroup(placement: .primaryAction) {
            Menu {
              if let chat = store.selectedChat, store.route == "chat" {
                Button("重命名聊天…") { store.renameChat(chat) }
                Button(chat.pinned ? "取消置顶" : "置顶聊天") {
                  store.updateChat(chat, field: "pinned", value: !chat.pinned)
                }
                Button("归档聊天") { store.updateChat(chat, field: "archived", value: true) }
                moveMenu(chat)
                Button("恢复与未决结果…") { recoveryVisible = true }
                Button("文件变更记录…") { fileHistoryVisible = true }
                Button("重新关联聊天工作区…", action: store.chooseChatWorkspace)
                  .disabled(chat.active || store.workspacePlanning || store.workspaceSaving)
                Button("删除聊天…", role: .destructive) { store.removeChat(chat) }
              }
            } label: {
              Image(systemName: "ellipsis")
            }
            .disabled(store.busy || store.selected == nil).accessibilityIdentifier("chat.more")
            if store.isProject {
              Button {
                store.inspectorVisible.toggle()
              } label: {
                Label("检查器", systemImage: "sidebar.right")
              }
            }
          }
        }
    }
    .onChange(of: store.draft) { _, _ in store.scheduleDraftSave() }
    .onChange(of: store.profiles) { _, profiles in
      if !profiles.isEmpty, store.selected == nil, store.route == "chat", !store.busy {
        store.reload()
      }
    }
    .onChange(of: store.selected) { old, new in store.selectChat(from: old, to: new) }
    .onChange(of: store.route) { _, route in
      if route == "chat", let id = store.selected { store.restoreChatMetadata(id) }
    }
    .onChange(of: store.identityEpoch) { _, _ in
      grantsVisible = false
      recoveryVisible = false
      fileHistoryVisible = false
    }
    .frame(minWidth: 1000, minHeight: 700)
    .environment(\.chatBodySize, CGFloat(store.generalPreferences.textSize.points + 2))
    .environment(\.chatHTMLPalette,store.themeHTMLPalette).preferredColorScheme(store.themeScheme).tint(store.themeTint).background(store.themeBackground).foregroundStyle(store.themeText)
    .sheet(isPresented: $store.mediaVisible) { MediaCallSetup() }
    .sheet(isPresented: $recoveryVisible) { RecoveryView() }
    .sheet(isPresented: $fileHistoryVisible) { FileHistoryListView() }
    .sheet(item: $store.selectedReceipt) { FileReceiptView(selection: $0) }
    .sheet(item: $store.remembering) { selection in MemoryEditor(selection: selection) }
    .sheet(item: $store.selectedChange) { change in FileDiffView(change: change) }
    .sheet(isPresented: $grantsVisible) { grantsPanel }
    .sheet(
      item: Binding(
        get: { store.editingProject?.revision == 0 ? store.editingProject : nil },
        set: { store.editingProject = $0 })
    ) { project in ProjectEditor(project: project) }
    .sheet(item: $store.openedDocument) { doc in ProjectDocumentView(document: doc) }
    .sheet(item: $store.skillScope) { SkillsSheet(target: $0) }
    .sheet(item: $store.modelSwitchPlan) { review in ModelSwitchView(review: review) }
    .sheet(item: $store.workspaceReview) { review in ChatWorkspaceView(review: review) }
    .sheet(item: $store.attachmentSelection) { selection in AttachmentPreviewView(selection: selection) }
  }
  private var toolEntries: [String] {
    store.activity
  }
  private var conversation: some View {
    ScrollViewReader { proxy in
      ScrollView {
        LazyVStack(alignment: .leading, spacing: 26) {
          ForEach(
            store.messages.filter {
              ($0.role == "user" || $0.role == "assistant" || $0.role == "tool")
                && (!$0.content.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
                  || (store.showReasoning
                    && ($0.reasoning?.isEmpty == false || $0.reasoningUnavailable == true)))
            }
          ) { m in
            Group {
              if m.media?["kind"] == "start" {
                Label("通话已开始",systemImage:"phone").font(.caption).foregroundStyle(.secondary).frame(maxWidth:.infinity)
              } else if m.role == "tool" {
                ToolChatCard(message: m)
              } else if m.role == "user" {
                HStack {
                  Spacer(minLength: 100)
                  VStack(alignment: .leading, spacing: 10) {
                    if let media=m.media {MediaSourceLabel(media:media)}
                    if !m.content.isEmpty {
                      Text(m.content).textSelection(.enabled).font(.system(size: store.generalPreferences.textSize.points))
                    }
                    if let attachment = m.attachment, let chat = store.selected {
                      AttachmentCard(attachment: attachment, chat: chat)
                    }
                  }.padding(
                    .horizontal, 18
                  ).padding(.vertical, 14).background(
                    Color.primary.opacity(0.045), in: RoundedRectangle(cornerRadius: 14))
                }
              } else {
                VStack(alignment: .leading, spacing: 12) {
                  ChatAssistantHeader(
                    showAvatar: !store.isProject, modelName: store.isProject ? nil : m.modelName)
                  VStack(alignment: .leading, spacing: 16) {
                    if let media=m.media {MediaSourceLabel(media:media)}
                    if store.showReasoning,
                      m.reasoning?.isEmpty == false || m.reasoningUnavailable == true
                    {
                      PublicReasoningView(
                        text: m.reasoning ?? "", kind: m.reasoningKind ?? "reasoning",
                        unavailable: m.reasoningUnavailable == true)
                    }
                    if !m.content.isEmpty {
                      ChatHTMLContent(source: m.content)
                    }
                  }.padding(18).frame(maxWidth: .infinity, alignment: .leading)
                    .background(store.themeSurface, in: RoundedRectangle(cornerRadius: 14))
                    .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(Color.primary.opacity(0.09)))
                    .padding(.leading, store.isProject ? 0 : 42)
                }
              }
            }.id(m.id).padding(store.jumpToMessage == m.sequence ? 8 : 0).background(
              store.jumpToMessage == m.sequence ? Color.accentColor.opacity(0.07) : .clear,
              in: RoundedRectangle(cornerRadius: 12)
            )
            .contextMenu {
              if !m.content.isEmpty {
                Button("复制消息") {
                  NSPasteboard.general.clearContents()
                  NSPasteboard.general.setString(m.content, forType: .string)
                }
                Button("保存为记忆候选…") { store.remember(m) }
              }
            }
          }
          if !store.live.isEmpty || (store.showReasoning && !store.liveReasoning.isEmpty) {
            VStack(alignment: .leading, spacing: 12) {
              ChatAssistantHeader(
                showAvatar: !store.isProject, modelName: store.isProject ? nil : store.liveModelName
              )
              VStack(alignment: .leading, spacing: 16) {
                if store.showReasoning, !store.liveReasoning.isEmpty {
                  PublicReasoningView(text: store.liveReasoning, kind: store.liveReasoningKind)
                }
                if !store.live.isEmpty { ChatHTMLContent(source: store.live) }
              }.padding(18).frame(maxWidth: .infinity, alignment: .leading)
                .background(store.themeSurface, in: RoundedRectangle(cornerRadius: 14))
                .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(Color.primary.opacity(0.09)))
                .padding(.leading, store.isProject ? 0 : 42)
            }
          }
          ForEach(store.changes) { change in
            Button {
              store.selectedChange = change
            } label: {
              HStack(spacing: 12) {
                Image(systemName: "doc.text").font(.title3)
                VStack(alignment: .leading, spacing: 5) {
                  Text(URL(fileURLWithPath: change.path).lastPathComponent).fontWeight(.medium)
                  Text("已修改 · 查看差异").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Image(systemName: "chevron.right").font(.caption)
              }
            }.buttonStyle(.plain).padding(16).background(
              Color.primary.opacity(0.035), in: RoundedRectangle(cornerRadius: 12)
            ).accessibilityIdentifier("file.diff")
          }
          if store.busy, !toolEntries.isEmpty {
            HStack(spacing: 10) {
              ProgressView().controlSize(.small)
              Text(toolEntries.last ?? "正在处理…").font(.caption).foregroundStyle(.secondary)
                .lineLimit(1)
              Spacer()
              Button("查看执行记录") { store.openToolRecords() }.font(.caption)
            }.padding(.vertical, 8)
          }
          Color.clear.frame(height: 1).id("end")
        }.frame(maxWidth: 800).padding(.horizontal, 36)
          .frame(maxWidth: .infinity).padding(.vertical, 32)
      }.onChange(of: store.live) { _, _ in proxy.scrollTo("end", anchor: .bottom) }
        .onChange(of: store.liveReasoning) { _, _ in proxy.scrollTo("end", anchor: .bottom) }
        .onChange(
          of: store.messages.map(\.id)
        ) { _, _ in
          if let sequence = store.jumpToMessage {
            proxy.scrollTo("message:\(sequence)", anchor: .center)
          } else {
            proxy.scrollTo("end", anchor: .bottom)
          }
        }
        .onChange(of: store.jumpToMessage) { _, sequence in
          if let sequence { proxy.scrollTo("message:\(sequence)", anchor: .center) }
        }
    }
  }
  private var composer: some View {
    VStack(spacing: 10) {
      if let chat=store.selectedChat, !store.pluginClients.isEmpty {PluginClientDock(chat:chat.id)}
      if let project = store.currentProject, !project.context.isEmpty {
        DisclosureGroup(
          "项目资料 · 本轮已选 \(store.selectedProjectReferences.count) / \(project.context.count)"
        ) {
          VStack(alignment: .leading, spacing: 8) {
            ForEach(project.context, id: \.self) { path in
              HStack {
                Toggle(
                  path,
                  isOn: Binding(
                    get: { store.selectedProjectReferences[path] != nil },
                    set: { store.selectProjectReference(project, path: path, enabled: $0) })
                )
                .toggleStyle(.checkbox).disabled(store.busy)
                Spacer()
                if let hash = store.selectedProjectReferences[path] {
                  Text(String(hash.prefix(8))).font(.caption.monospaced()).foregroundStyle(
                    .secondary)
                }
                Button("预览") { store.readProjectDocument(project, path: path) }.buttonStyle(.plain)
              }
            }
            Text("仅发送本轮勾选的当前版本；已发送的资料仍属于之前的聊天上下文。").font(.caption2).foregroundStyle(.secondary)
          }.padding(.top, 8)
        }.font(.caption).accessibilityIdentifier("chat.projectReferences")
      }
      HStack(spacing: 16) {
        Button("附件", action: store.chooseAttachment).buttonStyle(.plain).disabled(!store.canEditAttachment)
          .accessibilityIdentifier("attachment.choose")
        Button("截图", action: store.captureScreenshot).buttonStyle(.plain).disabled(!store.canEditAttachment)
          .help("由系统选择窗口或屏幕，最长边 1024 像素；预览后添加。")
          .accessibilityIdentifier("attachment.screenshot")
        Divider().frame(height: 14)
        Button("Skills") {
          if let id = store.selected { store.skillScope = SkillScope(scope: "chat", scopeID: id) }
        }.buttonStyle(.plain).disabled(store.selected == nil || store.busy).accessibilityIdentifier(
          "chat.skills")
        Button {
          grantsVisible = true
        } label: {
          Text("授权记录")
        }.buttonStyle(.plain)
        Spacer()
        Button { store.mediaVisible = true } label: { Label("通话",systemImage:"phone") }.disabled(store.selected == nil || (store.busy && store.mediaCallID == nil)).accessibilityIdentifier("media.open")
        Text(store.busy ? "正在处理…" : "").foregroundStyle(.secondary)
      }.font(.system(size: 12))
      if store.attachmentBusy { ProgressView("正在读取附件…").controlSize(.small).frame(maxWidth: .infinity, alignment: .leading) }
      if let attachment = store.pendingAttachment, let chat = store.selected {
        AttachmentCard(attachment: attachment, chat: chat, pending: true)
      }
      if !store.attachmentError.isEmpty {
        HStack(alignment: .top) {
          Text(store.attachmentError).font(.caption).foregroundStyle(.orange).textSelection(.enabled)
          Spacer()
          if !store.attachmentsLoaded { Button("重新读取", action: store.reloadPendingAttachment).font(.caption) }
        }
      }
      Divider()
      ComposerTextView(
        text: $store.draft, sendKey: store.generalPreferences.sendKey,
        textSize: store.generalPreferences.textSize.points - 1,
        enabled: store.selected != nil && !store.creatingChat,
        canSubmit: store.canSendMessage, submit: store.send, pastePNG: store.pasteAttachment,
        unsupportedImage: { store.attachmentError = "此版本支持粘贴 PNG。请先保存为 PNG，或使用截图按钮。" }
      )
      .frame(minHeight: 58, maxHeight: 98).accessibilityIdentifier("chat.input")
      Divider().overlay(Color.primary.opacity(0.035))
      HStack {
        if store.profiles.isEmpty {
          SettingsLink { Label("配置模型", systemImage: "cpu") }.simultaneousGesture(
            TapGesture().onEnded { store.settingsSection = "模型" })
        } else {
          Picker(
            "模型",
            selection: Binding(
              get: { store.requiresModelSelection ? "" : store.profileID },
              set: { store.previewModelSwitch(to: $0) })
          ) {
            if store.requiresModelSelection { Text("请选择模型").tag("") }
            ForEach(store.profiles) { Text($0.name).tag($0.id) }
          }.labelsHidden().pickerStyle(.menu).fixedSize().disabled(
            store.busy || store.creatingChat || store.switchingModel || store.checkingConnection
              || store.configSaving
              || store.selectedChat?.active == true
          )
          .accessibilityIdentifier("model.switch")
          if store.messages.contains(where: { $0.attachment?.format == "png" }) || store.pendingAttachment?.format == "png" {
            Button("图片传递…") { store.previewModelSwitch(to: store.profileID, allowSame: true) }
              .buttonStyle(.plain).font(.caption).disabled(store.busy || store.switchingModel || store.attachmentInteraction)
              .accessibilityIdentifier("model.imagePolicy")
          }
        }
        Spacer()
        Text(store.generalPreferences.sendKey.hint).font(.system(size: 11)).foregroundStyle(
          .secondary)
        if store.busy {
          Button(store.stopping ? "正在停止…" : "停止", action: store.stop).disabled(store.stopping)
            .accessibilityIdentifier("chat.stop")
        } else {
          Button("发送", action: store.send).buttonStyle(.borderedProminent)
            .disabled(!store.canSendMessage).accessibilityIdentifier("chat.send")
        }
      }
    }.padding(14).background(
      store.themeSurface, in: RoundedRectangle(cornerRadius: 12)
    ).overlay(RoundedRectangle(cornerRadius: 12).stroke(Color.primary.opacity(0.14), lineWidth: 1))
      .shadow(color: Color.black.opacity(store.themeScheme == .dark ? 0.20 : 0.07), radius: 7, x: 0, y: 4)
      .frame(maxWidth: 800).padding(.horizontal, 32).frame(maxWidth: .infinity)
      .padding(.bottom, 24).padding(.top, 8)
  }
  private var searchPane: some View {
    VStack(alignment: .leading, spacing: 12) {
      HStack {
        TextField("用一句话查找旧聊天", text: $store.search).onSubmit(store.find).textFieldStyle(
          .roundedBorder
        ).accessibilityIdentifier("search.input")
        Button("搜索", action: store.find)
      }
      if !store.search.isEmpty {
        ScrollView {
          ForEach(store.hits) { hit in
            Button {
              store.openChat(hit.chat, sequence: hit.sequence)
            } label: {
              Text(hit.text).lineLimit(3).frame(maxWidth: .infinity, alignment: .leading).padding(
                10)
            }.buttonStyle(.plain).disabled(store.busy || store.creatingChat || store.switchingModel)
          }
        }.frame(maxHeight: 170)
      }
    }.padding(20)
  }
  private var errorBanner: some View {
    HStack(alignment: .top) {
      Image(systemName: "exclamationmark.triangle")
      Text(store.error).textSelection(.enabled).font(.callout)
      Spacer()
      if !store.busy {
        if store.selectedChat?.active == true { Button("继续回合", action: store.resume) }
        Button {
          store.error = ""
        } label: {
          Image(systemName: "xmark")
        }.buttonStyle(.plain)
      }
    }.foregroundStyle(.secondary).padding(14).background(Color.orange.opacity(0.08)).padding(
      .horizontal, 32)
  }
  private var inspector: some View {
    VStack(alignment: .leading, spacing: 22) {
      HStack {
        Text("文件变更").fontWeight(.semibold)
        Spacer()
        Text("当前聊天").font(.caption).foregroundStyle(.secondary)
      }
      ScrollView {
        VStack(alignment: .leading, spacing: 14) {
          ForEach(latestFileReceipts) { receipt in
            Button {
              if let chat = store.selected {
                store.selectedReceipt = FileReceiptSelection(id: receipt.id, chat: chat)
              }
            } label: {
              HStack(alignment: .top, spacing: 10) {
                Image(
                  systemName: receipt.operation == "restore_file"
                    ? "arrow.uturn.backward" : "doc.text"
                ).foregroundStyle(receipt.color)
                VStack(alignment: .leading, spacing: 5) {
                  Text(receipt.path).lineLimit(2)
                  Text(receipt.status).font(.caption).foregroundStyle(receipt.color)
                }
                Spacer(minLength: 0)
              }
            }.buttonStyle(.plain).accessibilityIdentifier("inspector.receipt.\(receipt.id)")
          }
          if store.receiptRows.isEmpty {
            Text("受控文件工具的前后快照与回退记录会保存在这里。").font(.callout).foregroundStyle(.secondary)
          }
          Button("全部文件记录…") { fileHistoryVisible = true }.accessibilityIdentifier(
            "inspector.fileHistory")
          if !store.receiptError.isEmpty {
            Text(store.receiptError).font(.caption).foregroundStyle(.orange)
          }
          if !store.changes.isEmpty {
            DisclosureGroup("旧版差异快照") {
              Text("旧版界面快照未经持久回执验证，只读保留，不能直接回退。").font(.caption).foregroundStyle(.secondary)
              ForEach(store.changes) { change in
                Button(URL(fileURLWithPath: change.path).lastPathComponent) {
                  store.selectedChange = change
                }.buttonStyle(.plain)
              }
            }
          }
        }.frame(maxWidth: .infinity, alignment: .leading)
      }.frame(maxHeight: 280)
      Text("命令产生的额外文件变化需另行核对。").font(.caption).foregroundStyle(.secondary)
      Divider()
      Text("本次作业").fontWeight(.semibold)
      Text(store.busy ? "正在处理请求" : "等待下一步").font(.callout).foregroundStyle(.secondary)
      Spacer()
      Text("上下文").fontWeight(.semibold)
      Text("已保留 \(store.messages.count) 条记录").font(.callout).foregroundStyle(.secondary)
      Text("切换模型保留这段聊天").font(.caption).foregroundStyle(.secondary)
      Button("模型设置…") {
        store.settingsSection = "模型"
        openSettings()
      }
      .buttonStyle(.plain).foregroundStyle(.blue).accessibilityIdentifier(
        "inspector.modelSettings")
    }.padding(24).frame(maxHeight: .infinity, alignment: .top).onAppear { store.reloadReceipts() }
  }
  private var latestFileReceipts: [FileReceipt] {
    var seen = Set<String>()
    return Array(store.receiptRows.filter { seen.insert($0.path).inserted }.prefix(8))
  }
  private var grantsPanel: some View {
    VStack(alignment: .leading, spacing: 20) {
      HStack {
        Text("本次会话授权").font(.title2.weight(.semibold))
        Spacer()
        Button("完成") { grantsVisible = false }
      }
      Text("授权仅对当次工具调用有效，调用结束即失效。").foregroundStyle(.secondary)
      if store.busy { Button("停止当前调用", role: .destructive) { store.stop() } }
      ScrollView {
        VStack(alignment:.leading,spacing:14) {
          PlatformGrantList(chat:store.selected ?? "")
          if store.currentGrants.isEmpty {
            ContentUnavailableView("还没有工具授权记录",systemImage:"lock.shield",
              description:Text("本聊天中的工具授权会记录在这里。普通回复无需工具授权。")).frame(maxWidth:.infinity)
          }
          ForEach(store.currentGrants) { g in
            GroupBox {
              VStack(alignment:.leading,spacing:8) {
                HStack {Text(g.tool).fontWeight(.semibold);Spacer();Text(grantStatus(g)).foregroundStyle(.secondary)}
                Text(g.arguments).font(.system(size:12,design:.monospaced)).textSelection(.enabled)
                HStack {
                  Text(g.date,format:.dateTime)
                  Spacer()
                  if g.allowed,g.decided>0 {
                    if g.ended>0 {Text("授权持续 \(max(0,g.ended-g.decided)) 秒")}
                    else {Text(Date(timeIntervalSince1970:Double(g.decided)),style:.timer)}
                  }
                }.font(.caption).foregroundStyle(.secondary)
                HStack {
                  Text("确认等待 \(String(format:"%.1f",Double(g.waitMs)/1000)) 秒").font(.caption).foregroundStyle(.secondary)
                  Spacer()
                  Button(g.status == "issued_once" ? "撤销当前调用":"核对撤销状态") {store.revokeGrant(g)}.disabled(g.recordID == nil)
                }
              }.padding(8)
            }
          }
          if let id=store.currentGrants.last?.recordID,store.currentGrants.count>=200 {
            Button("加载更早的记录") {store.reloadGrants(chat:store.selected ?? "",before:id)}
          }
        }
      }

    }.padding(24).frame(width: 760, height: 560).onAppear {
      store.reloadGrants(chat: store.selected ?? "")
    }.onChange(of: store.busy) { _, _ in store.reloadGrants(chat: store.selected ?? "") }
  }
  private func chatRow(_ chat: Chat) -> some View {
    Button {
      store.openChat(chat.id)
    } label: {
      HStack {
        Text(chat.title).lineLimit(1)
        if chat.active {
          Image(systemName: "arrow.trianglehead.2.clockwise.rotate.90").foregroundStyle(.secondary)
        }
      }
      .frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
    }.buttonStyle(.plain).tag(chat.id).accessibilityIdentifier("chat.row.\(chat.id)")
      .contextMenu {
        Button("重命名…") { store.renameChat(chat) }
        Button(chat.pinned ? "取消置顶" : "置顶") {
          store.updateChat(chat, field: "pinned", value: !chat.pinned)
        }
        Button("归档") { store.updateChat(chat, field: "archived", value: true) }
        moveMenu(chat)
        Button("删除聊天…", role: .destructive) { store.removeChat(chat) }
      }
  }
  private func moveMenu(_ chat: Chat) -> some View {
    Menu("移动到") {
      Button("普通聊天") { store.updateChat(chat, field: "project", value: "") }
      ForEach(store.projects) { project in
        Button(project.name) { store.updateChat(chat, field: "project", value: project.id) }
          .disabled(project.id == chat.project || !project.available)
      }
    }
  }
}
struct FileDiffView: View {
  let change: FileChange
  @Environment(\.dismiss) private var dismiss
  var body: some View {
    VStack(alignment: .leading, spacing: 20) {
      HStack {
        Label(URL(fileURLWithPath: change.path).lastPathComponent, systemImage: "doc.text").font(
          .title3.weight(.semibold))
        Spacer()
        Text("旧版界面快照").foregroundStyle(.secondary)
        Button("完成") { dismiss() }
      }
      Text("这份旧版快照不证明文件已成功写入或保持不变，不能用于直接回退。").font(.caption).foregroundStyle(.secondary)
      HStack(alignment: .top, spacing: 0) {
        column("修改前", change.existed ? change.before : "（新文件）", .red)
        Divider()
        column("修改后", change.after, .green)
      }
    }.padding(24).frame(width: 920, height: 600)
  }
  private func column(_ title: String, _ text: String, _ color: Color) -> some View {
    VStack(alignment: .leading, spacing: 12) {
      Text(title).font(.headline)
      ScrollView([.horizontal, .vertical]) {
        Text(text).font(.system(size: 13, design: .monospaced)).textSelection(.enabled).frame(
          maxWidth: .infinity, alignment: .topLeading
        ).padding(14)
      }
    }.padding(12).frame(maxWidth: .infinity, maxHeight: .infinity).background(color.opacity(0.045))
  }
}
