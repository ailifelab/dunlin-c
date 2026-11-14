import AppKit
import SwiftUI

struct MainView: View {
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.openSettings) private var openSettings
  @State private var grantsVisible = false
  @State private var recoveryVisible = false
  @State private var fileHistoryVisible = false
  private var filePageVisible: Bool { fileHistoryVisible || store.selectedReceipt != nil }
  @State private var waitingChat: Chat?
  @State private var waitingText = ""
  @State private var waitingLoaded = false
  @State private var waitingDirty = false
  @State private var waitingReadError = ""
  @State private var waitingSave: DispatchWorkItem?
  var body: some View {
    NavigationSplitView {
      VStack(alignment: .leading, spacing: 0) {
        Text("Dunlin Copilot").font(.title2.weight(.semibold)).padding(.horizontal, 24)
          .padding(.top, 22).padding(.bottom, 18)
        List(selection: Binding<String?>(get:{waitingChat?.id ?? (store.route == "plugin" ? "plugin:"+store.pluginPageID:store.selected)},set:{value in
          guard let value else{return}
          closeFilePages()
          if store.busy {
            if let chat = store.chats.first(where: { $0.id == value }) { openWaitingChat(chat) }
            return
          }
          if value.hasPrefix("plugin:") {store.pluginPageID=String(value.dropFirst(7));store.route="plugin"}
          else if store.chats.contains(where:{$0.id==value}) {store.selected=value;store.route="chat"}
        })) {
          Section {
            Button {
              store.newChat()
            } label: {
              Label("新聊天", systemImage: "plus").font(.body.weight(.semibold))
                .imageScale(.medium).frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
            }.buttonStyle(.plain).foregroundStyle(.primary).accessibilityIdentifier("chat.new")
            Button {
              store.route = "search"
            } label: {
              Label("搜索", systemImage: "magnifyingglass").font(.body).imageScale(.medium)
                .frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
            }.buttonStyle(.plain).foregroundStyle(.primary).accessibilityIdentifier("chat.search")
            Button(action: store.browseProjects) {
              Label("项目", systemImage: "folder").font(.body).imageScale(.medium)
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
          }.disabled(store.busy)
          if !store.pluginPages.filter({$0.contribution.kind == "sidebar"}).isEmpty {
            Section("插件") {ForEach(store.pluginPages.filter {$0.contribution.kind == "sidebar"}) {page in
              Button {store.pluginPageID = page.id;store.route = "plugin"} label: {Label(page.contribution.title ?? page.package,systemImage:"puzzlepiece.extension").frame(maxWidth:.infinity,alignment:.leading).contentShape(Rectangle())}.tag("plugin:"+page.id).accessibilityIdentifier("plugin.page."+page.package)
            }}.disabled(store.busy)
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
                  open: { store.openProject(project) }).disabled(store.busy)
                  .frame(height: 25).contextMenu {
                    Button("项目主页") { store.openProject(project) }
                    Button("项目属性…") { store.editProject(project) }
                    Button("在访达中显示") { NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: project.path) }
                    Button("移除项目…", role: .destructive) { store.removeProject(project) }
                  }
                if project.expanded {
                  Button { store.newChat(workspace: project.path) } label: {
                    Text("新聊天").frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
                  }.padding(.leading, 18).disabled(store.busy || !project.available)
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
                  .help("新建项目").accessibilityIdentifier("project.add").disabled(store.busy)
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
        ).disabled((store.busy && store.ordinaryRunningChat == nil) || store.creatingChat || store.switchingModel || store.workspacePlanning || store.workspaceSaving || store.attachmentInteraction)
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
      ZStack {
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
          if let target = waitingChat, store.ordinaryRunningChat != nil { waitingDraft(target) }
          else if store.route == "search" {
            HistorySearchView().id(store.identityEpoch)
          } else if store.route == "plugin" {
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
                if showsCompactionState {
                  compactionState
                } else if store.messagesLoading && store.messages.isEmpty {
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
          if !store.error.isEmpty && !showsCompactionState && !showsFailedReply { errorBanner }
        }.frame(maxWidth: .infinity,maxHeight:.infinity)
        if waitingChat == nil && store.inspectorVisible && store.isProject && store.route == "chat"
          && store.conversationPage == "chat"
        {
          Divider()
          inspector.frame(width: 282)
        }
        }.opacity(filePageVisible ? 0 : 1).allowsHitTesting(!filePageVisible).accessibilityHidden(filePageVisible)
        if fileHistoryVisible {
          FileHistoryListView { fileHistoryVisible = false }
            .opacity(store.selectedReceipt == nil ? 1 : 0)
            .allowsHitTesting(store.selectedReceipt == nil).accessibilityHidden(store.selectedReceipt != nil)
        }
        if let selection = store.selectedReceipt {
          FileReceiptView(selection: selection) { store.selectedReceipt = nil }.id(selection.id)
        }
      }.background {PluginAppearanceBackdrop()}
        .navigationTitle(
          filePageVisible ? (store.selectedReceipt == nil ? "文件变更记录" : "文件变更") : store.route == "search" ? "搜索聊天" : store.route == "agents" ? "Agent 协作" : store.route == "tools" ? "工具执行记录" : store.route == "projects"
            ? "项目"
            : store.route == "archive"
              ? "已归档"
              : ["project", "projectSettings"].contains(store.route)
                ? (store.currentProject?.name ?? "项目") : (waitingChat?.title ?? store.selectedChat?.title ?? "新聊天")
        )
        .toolbar {
          ToolbarItemGroup(placement: .primaryAction) {
            Menu {
              ForEach(store.identities.filter { !$0.hidden }) { identity in
                Button(identity.name + (identity.selected ? " ✓" : "")) {
                  store.switchIdentity(identity)
                }
                .disabled(identity.selected || !store.canRequestIdentitySwitch)
              }
              Divider()
              Button("管理身份…") {
                store.settingsSection = "身份与家庭"
                openSettings()
              }
            } label: {
              Label(store.currentIdentity?.name ?? "本机身份", systemImage: "person.crop.circle")
            }
            .disabled(!store.canRequestIdentitySwitch)
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
          if store.selected != nil, store.route == "chat", !filePageVisible {
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
                Menu("导出聊天") {
                  ForEach(TranscriptFormat.allCases, id: \.self) { format in
                    Button(format.label) { store.exportTranscript(chat, format: format) }
                  }
                }.disabled(store.messagesLoading || store.messageReadError != nil)
                Button("恢复与未决结果…") { recoveryVisible = true }
                Button("文件变更记录…") { fileHistoryVisible = true }
                Button("重新关联聊天工作区…", action: store.chooseChatWorkspace)
                  .disabled(chat.active || store.workspacePlanning || store.workspaceSaving)
                Button("删除聊天…", role: .destructive) { store.removeChat(chat) }
              }
            } label: {
              Image(systemName: "ellipsis")
            }
            .disabled(store.busy || store.selected == nil || filePageVisible).accessibilityIdentifier("chat.more")
            if store.isProject && !filePageVisible {
              Button {
                store.inspectorVisible.toggle()
              } label: {
                Label("检查器", systemImage: "sidebar.right")
              }
            }
          }
        }
    }
    .onChange(of: store.busy) { _, active in
      guard !active, let target = waitingChat else { return }
      saveWaitingDraft()
      waitingChat = nil
      if store.pendingIdentitySwitch == nil { _ = store.openChat(target.id) }
    }
    .onChange(of: store.draft) { _, _ in store.scheduleDraftSave() }
    .onChange(of: store.profiles) { _, profiles in
      if !profiles.isEmpty, store.selected == nil, store.route == "chat", !store.busy {
        store.reload()
      }
    }
    .onChange(of: store.selected, initial: true) { old, new in
      if let old, old != new { closeFilePages() }
      store.selectChat(from: old, to: new)
    }
    .onChange(of: store.route) { _, route in
      closeFilePages()
      if route == "chat", let id = store.selected { store.restoreChatMetadata(id) }
    }
    .onChange(of: store.identityEpoch) { _, _ in
      waitingChat = nil; waitingSave?.cancel(); waitingDirty = false; waitingText = ""
      grantResultText = nil
      grantsVisible = false
      recoveryVisible = false
      fileHistoryVisible = false
    }
    .frame(minWidth: 1000, minHeight: 700)
    .environment(\.chatBodySize, CGFloat(store.generalPreferences.textSize.points))
    .environment(\.chatHTMLPalette,store.themeHTMLPalette).preferredColorScheme(store.themeScheme).tint(store.themeTint).background {PluginAppearanceBackdrop(conversation:false)}.foregroundStyle(store.themeText)
    .sheet(isPresented: $store.mediaVisible) { MediaCallSetup() }
    .sheet(isPresented: $recoveryVisible) { RecoveryView() }
    .sheet(item: $store.cleanupChat) { chat in ChatCleanupView(chat: chat) }
    .sheet(item: $store.remembering) { selection in MemoryEditor(selection: selection) }
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
              ($0.role == "user" || $0.role == "assistant" || $0.role == "tool" || $0.role == "reference")
                && (!$0.content.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
                  || (store.showReasoning
                    && ($0.reasoning?.isEmpty == false || $0.reasoningUnavailable == true)))
            }
          ) { m in
            Group {
              if m.media?["kind"] == "start" {
                Label("通话已开始",systemImage:"phone").font(.caption).foregroundStyle(.secondary).frame(maxWidth:.infinity)
              } else if m.role == "reference" {
                VStack(alignment:.leading,spacing:8) {
                  Label("检查点恢复说明",systemImage:"arrow.uturn.backward").font(.caption).foregroundStyle(.secondary)
                  Text(m.content).textSelection(.enabled).font(.callout)
                }.frame(maxWidth:.infinity,alignment:.leading).padding(14).background(store.themeSurface,in:RoundedRectangle(cornerRadius:12))
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
                        unavailable: m.reasoningUnavailable == true, sourceChat: store.selected, sequence: m.sequence)
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
          if let chat = store.selected {
            ForEach(latestFileReceipts) { receipt in
              Button {
                store.selectedReceipt = FileReceiptSelection(id: receipt.id, chat: chat)
              } label: {
                HStack(spacing: 12) {
                  Image(systemName: "doc.text").font(.title3)
                  VStack(alignment: .leading, spacing: 5) {
                    Text(receipt.path).fontWeight(.medium)
                    Text(receipt.status).font(.caption).foregroundStyle(receipt.color)
                  }
                  Spacer()
                  Text("查看差异").font(.callout)
                }
              }.buttonStyle(.plain).padding(16)
                .background(Color.primary.opacity(0.035), in: RoundedRectangle(cornerRadius: 12))
                .accessibilityIdentifier("chat.receipt." + receipt.id)
            }
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
          if showsFailedReply { failedReplyCard }
          Color.clear.frame(height: 1).id("end")
        }.frame(maxWidth: 800).padding(.horizontal, 36)
          .frame(maxWidth: .infinity).padding(.vertical, 32)
      }.onAppear {
        if let sequence = store.jumpToMessage {
          proxy.scrollTo("message:\(sequence)", anchor: .center)
        }
      }.onChange(of:showsFailedReply){_,visible in if visible {proxy.scrollTo("end",anchor:.bottom)}}
        .onChange(of: store.live) { _, _ in proxy.scrollTo("end", anchor: .bottom) }
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
        if let chat=store.selected { ChatCompanions(chat:chat) }
        Spacer()
        Button { store.mediaVisible = true } label: { Label("通话",systemImage:"phone") }.disabled(store.selected == nil || (store.busy && store.mediaCallID == nil)).accessibilityIdentifier("media.open")
        Text(store.busy ? "正在处理…" : "").foregroundStyle(.secondary)
      }.font(.callout)
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
        textSize: store.generalPreferences.textSize.points,
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
        Text(store.generalPreferences.sendKey.hint).font(.caption).foregroundStyle(
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
  private var showsCompactionState: Bool {
    guard store.route=="chat",store.conversationPage=="chat",let chat=store.selected else{return false}
    return store.compactingChat==chat || (store.compactionIssueChat==chat && !store.error.isEmpty)
  }

  @ViewBuilder private var compactionState: some View {
    let working=store.compactingChat != nil
    VStack(alignment:.leading,spacing:24) {
      Spacer(minLength:12)
      Text(working ? "正在整理较早的聊天…" : store.compactionCancelled ? "已取消上下文整理" : "上下文尚未整理完成")
        .font(.title.weight(.semibold))
      if working {
        Text("保留近期轮次，把较早内容整理成带来源的摘要。").foregroundStyle(.secondary)
        ProgressView().progressViewStyle(.linear)
        HStack {Text(store.stopping ? "正在取消整理…" : "正在等待整理结果").foregroundStyle(.secondary)
          Spacer()
          Button("取消整理",action:store.cancelCompaction).disabled(store.stopping).accessibilityIdentifier("context.cancel")
        }
        GroupBox {VStack(alignment:.leading,spacing:8){
          Text("完整聊天保存在本机").font(.headline)
          Text("摘要通过来源与预算检查后才会用于请求。").foregroundStyle(.secondary)
        }.frame(maxWidth:.infinity,alignment:.leading).padding(10)}
      } else {
        Text(store.error).textSelection(.enabled).foregroundStyle(.secondary)
        VStack(alignment: .leading, spacing: 9) {
          Text("原始聊天已保留").font(.headline)
          Text(store.compactionCancelled ? "已发起的摘要请求可能产生费用，可在运行轨迹中查看。" : "这次未采用不完整的摘要，历史工具不会重新执行。")
          if !store.compactionCancelled { Text("可以重试整理、选择其他模型或调整保留的轮次。") }
        }
        .font(.callout).padding(18).frame(maxWidth: .infinity, alignment: .leading)
        .background(store.compactionCancelled ? Color.secondary.opacity(0.07) : Color.orange.opacity(0.11),
                    in: RoundedRectangle(cornerRadius: 10))
        HStack {
          if store.compactionCancelled {
            Button("返回聊天"){store.error="";store.compactionIssueChat=nil}
              .accessibilityIdentifier("context.return")
          } else {
            if store.failedTurn?.chat==store.selected {
              Button("重试整理",action:store.retryFailedTurn).buttonStyle(.borderedProminent)
                .accessibilityIdentifier("context.retry")
            }
            Menu("换模型…"){ForEach(store.profiles){profile in
              Button(profile.name){store.previewModelSwitch(to:profile.id)}.disabled(profile.id==store.profileID)
            }}.fixedSize().disabled(store.selectedChat?.active == true)
          }
          Button(store.compactionCancelled ? "更改整理策略…" : "选择保留的内容…"){
            store.settingsSection="上下文压缩";openSettings()
          }
          if !store.compactionCancelled {Button("返回聊天"){store.compactionIssueChat=nil;store.error=""}}
        }
      }
      Spacer(minLength:12)
    }.padding(.horizontal,34).frame(maxWidth:.infinity,maxHeight:.infinity,alignment:.leading)
  }

  private var showsFailedReply:Bool {
    store.failedTurn?.chat == store.selected && store.failedTurn != nil &&
      store.route == "chat" && store.conversationPage == "chat" && !store.error.isEmpty && !showsCompactionState
  }
  private var failedReplyCard:some View {
    VStack(alignment:.leading,spacing:12) {
      Label("模型请求未完成",systemImage:"exclamationmark.circle").font(.headline)
      Text(store.error).font(.callout).textSelection(.enabled)
      Text("已提交的消息保留在本机，可以检查服务商连接后重试。").foregroundStyle(.secondary)
      HStack {
        Button("重试",action:store.retryFailedTurn).accessibilityIdentifier("chat.failure.retry")
        Button("检查服务商"){store.settingsSection="服务商";openSettings()}
      }.disabled(store.busy)
    }.padding(20).frame(maxWidth:.infinity,alignment:.leading)
      .background(Color.red.opacity(0.07),in:RoundedRectangle(cornerRadius:12))
  }
  private var errorBanner: some View {
    HStack(alignment: .top) {
      Image(systemName: "exclamationmark.triangle")
      Text(store.error).textSelection(.enabled).font(.callout)
      Spacer()
      if !store.busy {
        if let failedTurn=store.failedTurn,failedTurn.chat==store.selected {
          Button("重试",action:store.retryFailedTurn)
          Button("检查服务商"){store.settingsSection="服务商";openSettings()}
        }
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
                store.selectedReceipt = FileReceiptSelection(id: receipt.id, chat: chat, origin: .inspector)
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
  @State private var grantResultText: ToolGrantReceipt?
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
                Text(g.arguments).font(.system(.callout, design: .monospaced)).textSelection(.enabled)
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
                  Button(g.status == "issued_once" ? "撤销当前调用":"核对撤销状态") {store.revokeGrant(g) { grantResultText = $0 }}.disabled(g.recordID == nil)
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
    }.grantResult($grantResultText).onChange(of: store.busy) { _, _ in store.reloadGrants(chat: store.selected ?? "") }
  }
  private func openWaitingChat(_ chat: Chat) {
    guard let running = store.ordinaryRunningChat, waitingChat?.id != chat.id else { return }
    saveWaitingDraft()
    waitingChat = chat.id == running ? nil : chat
    waitingReadError = ""
    waitingDirty = false
    guard waitingChat != nil else { return }
    waitingText = store.drafts[chat.id] ?? ""
    waitingLoaded = store.drafts[chat.id] != nil
    if !waitingLoaded {
      store.readLocalView(["op":"chats.metadata","id":chat.id], as: ChatMetadata.self) { result in
        guard waitingChat?.id == chat.id else { return }
        if let metadata = result.data {
          waitingText = metadata.draft
          waitingLoaded = true
        } else { waitingReadError = "草稿暂不可读，返回后可重试。" }
      }
    }
  }

  private func saveWaitingDraft() {
    waitingSave?.cancel()
    guard let chat = waitingChat, waitingLoaded, waitingDirty else { return }
    waitingDirty = false
    store.drafts[chat.id] = waitingText
    store.workspaceRequest(["op":"chats.update", "id":chat.id, "field":"draft", "value":waitingText],
                           as: NoData.self) { result in
      if !result.ok { store.error = store.workspaceError(result.error) }
    }
  }

  private func waitingDraft(_ chat: Chat) -> some View {
    VStack(alignment: .leading, spacing: 16) {
      Spacer()
      Text("等待当前回复完成").font(.title.weight(.semibold))
      let title = store.chats.first { $0.id == store.ordinaryRunningChat }?.title ?? "当前聊天"
      Text("「" + title + "」正在生成回复。你可以继续编辑这段聊天的草稿。")
        .foregroundStyle(.secondary)
      Button("返回正在运行的聊天") {
        saveWaitingDraft()
        waitingChat = nil
      }.accessibilityIdentifier("chat.returnToRunning")
      if store.stopping {
        VStack(alignment: .leading, spacing: 7) {
          Text("正在停止…").font(.headline)
          Text("正在等待当前请求结束。").foregroundStyle(.secondary)
        }.padding(16).frame(maxWidth: .infinity, alignment: .leading)
          .background(.quaternary, in: RoundedRectangle(cornerRadius: 10))
      }
      Spacer()
      if !waitingReadError.isEmpty { Text(waitingReadError).foregroundStyle(.orange) }
      Text("回复完成后，请手动发送这段草稿。").font(.caption).foregroundStyle(.secondary)
      VStack(alignment: .trailing, spacing: 8) {
        TextEditor(text: Binding(get: { waitingText }, set: { value in
          waitingText = value
          waitingDirty = true
          waitingSave?.cancel()
          let save = DispatchWorkItem { saveWaitingDraft() }
          waitingSave = save
          DispatchQueue.main.asyncAfter(deadline: .now() + 0.35, execute: save)
        }))
        .disabled(!waitingLoaded).scrollContentBackground(.hidden)
        .frame(minHeight: 60, maxHeight: 100).accessibilityIdentifier("chat.waitingDraft")
        Button("发送") {}.disabled(true)
      }.padding(12).background(store.themeSurface, in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(.quaternary))
        .shadow(color: .black.opacity(0.07), radius: 6, y: 3)
    }.padding(32).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
  }

  private func closeFilePages() {
    fileHistoryVisible = false
    store.selectedReceipt = nil
  }

  private func chatRow(_ chat: Chat) -> some View {
    Button {
      closeFilePages()
      if store.busy { openWaitingChat(chat) } else { store.openChat(chat.id) }
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
private struct HistorySearchView: View {
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.openSettings) private var openSettings
  @FocusState private var focused: Bool
  @State private var preview: SearchHit?
  @State private var excerpt: [SourceLine] = []
  @State private var previewError = ""
  @State private var loadingSource = false
  @State private var sourceRequest = UUID()

  private struct SourceLine: Decodable {
    let sequence: Int
    let role: String
    let text: String
    let time: Int64?
  }

  var body: some View {
    Group {
      if let hit = preview { sourcePage(hit) } else { results }
    }
    .padding(.horizontal, 32).padding(.vertical, 22)
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    .background(store.themeBackground)
  }

  private func scope(_ hit: SearchHit) -> String {
    let project = store.chats.first { $0.id == hit.chat }?.project
    return store.projects.first { $0.id == project }?.name ?? "普通聊天"
  }

  private func speaker(_ role: String) -> String {
    switch role {
    case "user": return store.currentIdentity?.name ?? "用户"
    case "assistant": return "助手"
    case "tool": return "工具"
    default: return role
    }
  }

  private func timestamp(_ time: Int64?) -> String? {
    guard let time else { return nil }
    return Date(timeIntervalSince1970: Double(time) / 1000).formatted(date: .abbreviated, time: .shortened)
  }

  private func readSource(_ hit: SearchHit, open: Bool = false) {
    let request = UUID()
    sourceRequest = request
    loadingSource = true
    previewError = ""
    var args: [String: Any] = ["op": "index.source", "chat": hit.chat, "sequence": hit.sequence,
                               "text": hit.text, "role": hit.role]
    if let time = hit.time { args["time"] = time }
    store.workspaceRequest(args, as: [SourceLine].self) { reply in
      guard preview?.id == hit.id, sourceRequest == request else { return }
      loadingSource = false
      guard reply.ok, let lines = reply.data, lines.contains(where: { $0.sequence == hit.sequence }) else {
        excerpt = []
        previewError = reply.error == "source_changed" ? "原文已变化或已删除，请返回并重新搜索。" : "无法读取聊天原文，请返回重试。"
        return
      }
      excerpt = lines
      if open { _ = store.openChat(hit.chat, sequence: hit.sequence) }
    }
  }

  private func sourcePage(_ hit: SearchHit) -> some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 18) {
        Button {
          sourceRequest = UUID()
          preview = nil
        } label: { Label("返回搜索结果", systemImage: "chevron.left") }
          .buttonStyle(.borderless).accessibilityIdentifier("search.return")
        Text("命中的原话").font(.title2.bold())
        Text([store.chats.first { $0.id == hit.chat }?.title, scope(hit), store.currentIdentity?.name]
          .compactMap { $0 }.joined(separator: " · ")).foregroundStyle(.secondary)
        if !previewError.isEmpty {
          Label(previewError, systemImage: "exclamationmark.triangle").foregroundStyle(.secondary)
        } else if loadingSource && excerpt.isEmpty {
          ProgressView("正在读取原文…")
        }
        ForEach(excerpt, id: \.sequence) { line in
          VStack(alignment: .leading, spacing: 14) {
            Text([timestamp(line.time), speaker(line.role), line.sequence == hit.sequence ? "命中原文" : (line.sequence > hit.sequence ? "后续消息" : "前文")]
              .compactMap { $0 }.joined(separator: " · "))
              .font(.subheadline.weight(.semibold)).foregroundStyle(line.sequence == hit.sequence ? Color.accentColor : .secondary)
            Text(line.text).font(.body).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
          }.padding(20).frame(maxWidth: .infinity, minHeight: 100, alignment: .leading)
            .background(store.themeSurface, in: RoundedRectangle(cornerRadius: 12))
        }
        if !excerpt.isEmpty {
          Text("这里按原来的顺序展示命中消息和相邻原文。更早或更晚的补充可以在聊天中查看。")
            .padding(18).frame(maxWidth: .infinity, alignment: .leading)
            .background(Color.accentColor.opacity(0.08), in: RoundedRectangle(cornerRadius: 10))
        }
        Button("在聊天中打开") { readSource(hit, open: true) }.buttonStyle(.borderedProminent)
          .controlSize(.large).disabled(excerpt.isEmpty || loadingSource || store.busy || store.workspaceTransitioning)
          .accessibilityIdentifier("search.open-chat")
      }.frame(maxWidth: .infinity, alignment: .leading)
    }
  }

  private var results: some View {
    VStack(alignment: .leading, spacing: 16) {
      HStack(spacing: 12) {
        Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
        TextField("用一句话查找旧聊天", text: $store.search)
          .textFieldStyle(.plain).onSubmit(store.find).focused($focused)
          .accessibilityIdentifier("search.input")
        Button("搜索", action: store.find).buttonStyle(.borderedProminent).controlSize(.large)
          .disabled(store.busy || store.search.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
          .accessibilityIdentifier("search.submit")
      }.padding(.horizontal, 14).frame(minHeight: 46)
        .background(store.themeSurface, in: RoundedRectangle(cornerRadius: 10))
      HStack(spacing: 12) {
        Label(store.currentIdentity?.name ?? "当前身份", systemImage: "person.crop.circle")
        Picker("项目", selection: $store.searchProject) {
          Text("所有项目").tag("")
          ForEach(store.projects) { Text($0.name).tag($0.id) }
        }.labelsHidden().fixedSize().accessibilityIdentifier("search.project")
        Picker("时间", selection: $store.searchDays) {
          Text("不限时间").tag(0); Text("最近七天").tag(7); Text("最近三十天").tag(30)
        }.labelsHidden().fixedSize().accessibilityIdentifier("search.time")
        Spacer()
        Button("检索设置…") { store.settingsSection = "聊天检索"; openSettings() }.buttonStyle(.borderless)
      }
      if let pending = store.indexStatus?.pending, pending > 0 {
        Text("\(pending) 段新内容正在整理，当前结果可能不完整。").padding(14)
          .frame(maxWidth: .infinity, alignment: .leading)
          .background(Color.accentColor.opacity(0.08), in: RoundedRectangle(cornerRadius: 9))
      }
      HStack {
        Text("按词语与短语匹配").font(.headline)
        Spacer()
        Text("本地语义检索未开启").font(.caption).foregroundStyle(.secondary)
      }
      if let error = store.searchFailure {
        Label(error, systemImage: "exclamationmark.triangle")
      } else if store.searchedQuery != nil && store.hits.isEmpty {
        VStack(spacing: 14) {
          Label("没有找到相关聊天", systemImage: "magnifyingglass").font(.headline)
          Text("试试其他关键词，或移除项目和时间筛选。").foregroundStyle(.secondary)
          if !store.searchProject.isEmpty || store.searchDays != 0 {
            Button("清除筛选") { store.searchProject = ""; store.searchDays = 0 }
              .accessibilityIdentifier("search.clear-filters")
          } else {
            Button("修改关键词") { focused = true }
          }
        }.padding(24).frame(maxWidth: .infinity, minHeight: 170)
          .background(store.themeSurface, in: RoundedRectangle(cornerRadius: 11))
        if let project = store.projects.first(where: { $0.id == store.searchProject }),
           !store.chats.contains(where: { $0.project == project.id && !$0.archived }) {
          VStack(alignment: .leading, spacing: 12) {
            Text(project.name).font(.headline)
            Text("这个项目还没有聊天。从一个问题开始，项目上下文会跟随新的对话。").foregroundStyle(.secondary)
            Button("开始聊天") { store.newChat(workspace: project.path) }.disabled(!project.available)
          }.padding(24).frame(maxWidth: .infinity, alignment: .leading)
            .background(store.themeSurface, in: RoundedRectangle(cornerRadius: 11))
        }
      } else {
        ScrollView {
          LazyVStack(alignment: .leading, spacing: 14) {
            ForEach(store.hits) { hit in
              VStack(alignment: .leading, spacing: 8) {
                Text(store.chats.first { $0.id == hit.chat }?.title ?? "聊天记录").font(.headline)
                Text([scope(hit), speaker(hit.role), timestamp(hit.time)].compactMap { $0 }.joined(separator: " · "))
                  .font(.subheadline).foregroundStyle(.secondary)
                Text(hit.text).lineLimit(4).textSelection(.enabled)
                HStack {
                  if !hit.matchedTerms.isEmpty {
                    Text("匹配：" + hit.matchedTerms.joined(separator: "、"))
                      .font(.caption).foregroundStyle(.secondary).lineLimit(2)
                  }
                  Spacer()
                  Button("查看原文…") {
                    preview = hit
                    excerpt = []
                    readSource(hit)
                  }.disabled(store.busy || store.creatingChat || store.switchingModel)
                    .accessibilityIdentifier("search.source.\(hit.id)")
                }
              }.padding(18).frame(maxWidth: .infinity, alignment: .leading)
                .background(store.themeSurface, in: RoundedRectangle(cornerRadius: 11))
            }
          }
        }
      }
      Spacer(minLength: 0)
      Text("没有找到时，可以换一种说法或调整范围。").font(.caption).foregroundStyle(.secondary)
    }.onAppear { focused = true; store.reloadIndexStatus() }
      .onChange(of: store.searchProject) { _, _ in refreshFilter() }
      .onChange(of: store.searchDays) { _, _ in refreshFilter() }
  }

  private func refreshFilter() {
    store.hits = []
    store.searchedQuery = nil
    store.searchFailure = nil
    if !store.search.isEmpty { store.find() }
  }
}
