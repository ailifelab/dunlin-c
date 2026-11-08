import AppKit
import SwiftUI

struct ProjectsView: View {
  @EnvironmentObject var store: WorkspaceStore
  @State private var query = ""
  var body: some View {
    VStack(alignment: .leading, spacing: 24) {
      HStack {
        VStack(alignment: .leading, spacing: 7) {
          Text("你的项目").font(.system(size: 27, weight: .semibold))
          Text("把文件夹、项目指令和相关聊天放在一起。").foregroundStyle(.secondary)
        }
        Spacer()
        Button(action: store.addProject) { Label("新建项目", systemImage: "plus") }
          .buttonStyle(.borderedProminent).accessibilityIdentifier("projects.create")
      }
      TextField("搜索项目名称或路径", text: $query).textFieldStyle(.roundedBorder)
      if store.projects.isEmpty {
        ContentUnavailableView {
          Label("还没有项目", systemImage: "folder")
        } description: {
          Text("选择一个本地文件夹开始，无需 Git 仓库。")
        } actions: {
          Button("新建项目", action: store.addProject)
        }
      } else if !store.projects.contains(where: { matches($0) }) {
        ContentUnavailableView {
          Label("没有找到匹配的项目", systemImage: "magnifyingglass")
        } description: {
          Text("试试其他项目名称或路径。")
        } actions: {
          Button("清除搜索") { query = "" }
        }
      } else {
        ScrollView {
          LazyVGrid(columns: [GridItem(.adaptive(minimum: 300), spacing: 18)], spacing: 18) {
            ForEach(
              store.projects.filter { matches($0) }
            ) { project in
              VStack(alignment: .leading, spacing: 14) {
                HStack {
                  Image(systemName: project.available ? "folder" : "folder.badge.questionmark")
                    .font(.title2).foregroundStyle(project.available ? Color.accentColor : .orange)
                  Spacer()
                  Menu {
                    Button("项目属性…") { store.editProject(project) }
                    Button("在访达中显示") {
                      NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: project.path)
                    }
                    Button("移除项目…", role: .destructive) { store.removeProject(project) }
                  } label: {
                    Image(systemName: "ellipsis")
                  }.menuStyle(.borderlessButton).fixedSize()
                }
                Button {
                  store.openProject(project)
                } label: {
                  VStack(alignment: .leading, spacing: 9) {
                    Text(project.name).font(.headline)
                    Text(project.path).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                    Text(
                      project.available
                        ? "\(project.chatCount) 段聊天 · \(project.context.count) 份资料"
                        : project.availability
                    )
                    .font(.caption).foregroundStyle(project.available ? Color.secondary : .orange)
                  }.frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
                }.buttonStyle(.plain).accessibilityIdentifier("projects.open.\(project.id)")
              }.padding(20).frame(maxWidth: .infinity, minHeight: 155, alignment: .topLeading)
                .background(Color.primary.opacity(0.035), in: RoundedRectangle(cornerRadius: 13))
            }
          }
        }
      }
      Spacer(minLength: 0)
    }.padding(32)
  }
  private func matches(_ project: ProjectRecord) -> Bool {
    query.isEmpty || project.name.localizedCaseInsensitiveContains(query)
      || project.path.localizedCaseInsensitiveContains(query)
  }
}
struct ProjectHomeView: View {
  @EnvironmentObject var store: WorkspaceStore
  let project: ProjectRecord
  @State private var showingFiles = false
  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 26) {
        HStack {
          VStack(alignment: .leading, spacing: 10) {
            Text(project.name).font(.system(size: 27, weight: .semibold))
            Text(project.path).font(.callout).foregroundStyle(.secondary).textSelection(.enabled)
          }
          Spacer()
          Button("项目设置") { store.editProject(project) }.accessibilityIdentifier(
            "project.properties")
          Button("新聊天") { store.newChat(workspace: project.path) }.buttonStyle(.borderedProminent)
            .disabled(!project.available).accessibilityIdentifier("project.home.newChat")
        }
        HStack(spacing: 16) {
          Label(
            project.available ? "目录可访问" : project.availability,
            systemImage: project.available ? "checkmark.circle" : "exclamationmark.triangle"
          )
          .foregroundStyle(project.available ? Color.green : Color.orange)
          Label(
            project.policy == "readOnly"
              ? "只读工具" : project.policy == "noTools" ? "不使用工具" : "可编辑 · 可执行",
            systemImage: "lock.shield"
          ).foregroundStyle(.secondary)
          if !project.available {
            Button("重新选择文件夹…") { store.chooseProjectDefaultFolder(project) }
            Button("重试访问", action: store.reloadProjects)
          }
        }.font(.callout)
        HStack(spacing: 24) {
          tab("聊天", page: "chats")
          tab("上下文  \(project.context.count)", page: "context")
          Spacer()
        }.overlay(alignment: .bottom) { Divider() }
        if store.projectPage == "context" { contextContent } else { chatsContent }
      }.padding(32)
    }.accessibilityIdentifier("project.home")
      .onChange(of: project.id) { _, _ in showingFiles = false }
  }
  private func tab(_ title: String, page: String) -> some View {
    Button {
      store.projectPage = page
      if page == "context" { store.reloadProjectReferences(project) }
    } label: {
      Text(title).fontWeight(store.projectPage == page ? .semibold : .regular)
        .foregroundStyle(store.projectPage == page ? Color.accentColor : Color.secondary)
        .padding(.vertical, 14).overlay(alignment: .bottom) {
          if store.projectPage == page { Rectangle().fill(Color.accentColor).frame(height: 2) }
        }
    }.buttonStyle(.plain).accessibilityIdentifier("project.tab.\(page)")
  }
  private var chatsContent: some View {
    VStack(alignment: .leading, spacing: 18) {
      let chats = store.chats.filter { $0.project == project.id && !$0.archived }
      if chats.isEmpty {
        ContentUnavailableView(
          "还没有项目聊天", systemImage: "bubble.left.and.bubble.right",
          description: Text("新建一段聊天，一起处理这个项目。"))
      }
      ForEach(chats) { chat in
        HStack(spacing: 14) {
          Button {
            store.openChat(chat.id)
          } label: {
            VStack(alignment: .leading, spacing: 10) {
              Label(chat.title, systemImage: chat.pinned ? "pin" : "bubble.left")
                .font(.system(size: 16, weight: .medium))
              Text(
                "\(store.profiles.first(where: {$0.id == chat.model})?.name ?? "跟随默认模型") · \(chat.statusLabel)"
              )
              .font(.caption).foregroundStyle(.secondary)
            }.frame(maxWidth: .infinity, alignment: .leading).contentShape(Rectangle())
          }.buttonStyle(.plain).accessibilityIdentifier("project.chat.\(chat.id)")
          if let updated = chat.updated, updated > 0 {
            Text(Date(timeIntervalSince1970: Double(updated)), style: .relative).font(.caption)
              .foregroundStyle(.secondary)
          }
          Menu {
            Button("重命名…") { store.renameChat(chat) }
            Button(chat.pinned ? "取消置顶" : "置顶") {
              store.updateChat(chat, field: "pinned", value: !chat.pinned)
            }
            Menu("移动到…") {
              Button("普通聊天") { store.updateChat(chat, field: "project", value: "") }
              ForEach(store.projects.filter { $0.id != project.id }) { destination in
                Button(destination.name) {
                  store.updateChat(chat, field: "project", value: destination.id)
                }
              }
            }
            Button("归档") { store.updateChat(chat, field: "archived", value: true) }
            Button("删除聊天…", role: .destructive) { store.removeChat(chat) }
          } label: {
            Image(systemName: "ellipsis")
          }.menuStyle(.borderlessButton).fixedSize()
        }.padding(24).frame(minHeight: 92)
          .background(Color.primary.opacity(0.03), in: RoundedRectangle(cornerRadius: 12))
      }
      if !project.instructions.isEmpty {
        Text("项目指令已启用 · 下一次请求使用最新保存的版本").font(.caption).foregroundStyle(.secondary).padding(
          .top, 24)
      }
    }
  }
  private var contextContent: some View {
    VStack(alignment: .leading, spacing: 22) {
      HStack {
        Text("项目上下文").font(.title2.weight(.semibold))
        Spacer()
        Button {
          showingFiles.toggle()
          store.listProjectFiles(project.id, path: "")
        } label: {
          Label("添加项目文件", systemImage: "plus")
        }
        .disabled(!project.available).accessibilityIdentifier("project.context.add")
        Button {
          store.reloadProjectReferences(project)
        } label: {
          Image(systemName: "arrow.clockwise")
        }
        .help("检查文件版本").accessibilityIdentifier("project.context.refresh")
      }
      VStack(alignment: .leading, spacing: 10) {
        Text("新聊天会列出这些资料，发送前可以逐项选择。")
        Text("只在本轮确认使用后读取和发送；不会后台持续上传。")
      }.font(.callout).foregroundStyle(.secondary).padding(20).frame(
        maxWidth: .infinity, alignment: .leading
      )
      .background(Color.accentColor.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
      if project.context.isEmpty { Text("还没有固定资料。添加项目中的文本文件，供聊天选择。").foregroundStyle(.secondary) }
      ForEach(project.context, id: \.self) { path in
        let row = store.projectReferences.first { $0.path == path }
        HStack(spacing: 18) {
          VStack(alignment: .leading, spacing: 10) {
            Label(path, systemImage: "doc.text").font(.headline)
            Text(row?.message ?? "正在检查文件版本…").font(.caption)
              .foregroundStyle(
                row?.state == "changed" || row?.state == "unavailable"
                  ? Color.orange : Color.secondary)
          }.frame(maxWidth: .infinity, alignment: .leading)
          Button("预览") { store.readProjectDocument(project, path: path) }.disabled(
            row?.currentSHA == nil)
          if row?.state == "changed" || row?.state == "unverified" {
            Button("确认当前版本") { store.acknowledgeProjectReference(project, path: path) }
              .disabled(
                store.referencePreviews[path]?.sha256 != row?.currentSHA || row?.currentSHA == nil)
          }
          Button("移除引用") { store.unpinContext(project, path: path) }
        }.padding(22).frame(minHeight: 100)
          .background(Color.primary.opacity(0.03), in: RoundedRectangle(cornerRadius: 12))
      }
      if showingFiles { filesBrowser }
      Text("资料内容是参考信息，不会自动变成高优先级项目指令。").font(.caption).foregroundStyle(.secondary)
      Button("‹  返回项目聊天") { store.projectPage = "chats" }.buttonStyle(.plain)
    }.accessibilityIdentifier("project.context")
  }
  private var filesBrowser: some View {
    GroupBox {
      VStack(alignment: .leading, spacing: 12) {
        HStack {
          Label(store.fileDirectory.isEmpty ? "项目文件" : store.fileDirectory, systemImage: "folder")
          Spacer()
          if !store.fileDirectory.isEmpty {
            Button("上一级") {
              store.listProjectFiles(
                project.id,
                path: store.fileDirectory.split(separator: "/").dropLast().joined(separator: "/"))
            }
          }
          Button {
            store.listProjectFiles(project.id, path: store.fileDirectory)
          } label: {
            Image(systemName: "arrow.clockwise")
          }
        }
        Divider()
        if store.projectFiles.isEmpty { Text("此文件夹为空，或没有可展示的普通文件。").foregroundStyle(.secondary) }
        ForEach(store.projectFiles) { file in
          HStack {
            Button {
              if file.directory {
                store.listProjectFiles(project.id, path: file.path)
              } else {
                store.readProjectDocument(project, path: file.path)
              }
            } label: {
              Label(file.name, systemImage: file.directory ? "folder" : "doc.text")
            }
            .buttonStyle(.plain).frame(maxWidth: .infinity, alignment: .leading)
            if !file.directory {
              Text(ByteCountFormatter.string(fromByteCount: file.size, countStyle: .file)).font(
                .caption
              ).foregroundStyle(.secondary)
              Button(project.context.contains(file.path) ? "已固定" : "固定资料") {
                store.pinContext(project, path: file.path)
              }
              .disabled(project.context.contains(file.path))
            }
          }.padding(.vertical, 5)
        }
        Text("按目录读取，最多 1000 项。不跟随符号链接。").font(.caption).foregroundStyle(.secondary)
      }.padding(12)
    }.disabled(!project.available)
  }
}
struct ProjectEditor: View {
  @EnvironmentObject var store: WorkspaceStore
  @Environment(\.dismiss) private var dismiss
  @Environment(\.openSettings) private var openSettings
  let embedded: Bool
  @State private var draft: ProjectRecord
  @State private var relink = false
  @State private var section = "基本信息"
  @State private var ownerEpoch: Int64?
  init(project: ProjectRecord, embedded: Bool = false) {
    _draft = State(initialValue: project)
    self.embedded = embedded
  }
  var body: some View {
    Group {
      if embedded {
        content.padding(32).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
      } else {
        content.padding(28).frame(width: 740, height: 660)
      }
    }.disabled(store.projectSaving || store.busy).onAppear { ownerEpoch = store.identityEpoch }
  }
  private var content: some View {
    VStack(alignment: .leading, spacing: 20) {
      Text(draft.revision == 0 ? "新建项目" : "项目属性").font(.title2.weight(.semibold))
      if draft.revision == 0 {
        Text("将相关聊天和资料放在一起。").font(.callout).foregroundStyle(.secondary)
      }
      if draft.revision > 0 {
        Picker("配置", selection: $section) {
          Text("基本信息").tag("基本信息")
          Text("Skills").tag("Skills")
          Text("协作与权限").tag("协作与权限")
        }.pickerStyle(.segmented).labelsHidden().accessibilityIdentifier("project.properties.tab")
      }
      if section == "基本信息" {
        ScrollView {
          VStack(alignment: .leading, spacing: 24) {
            field("项目名称") {
              TextField("项目名称", text: $draft.name).accessibilityIdentifier("project.name")
            }
            field("本地文件夹") {
              HStack {
                TextField("尚未选择", text: $draft.path).accessibilityIdentifier("project.path")
                Button("选择文件夹…", action: selectFolder).accessibilityIdentifier(
                  "project.chooseFolder")
              }
            }
            Text("文件夹需与应用数据目录分开；不会自动扫描或上传整个目录。").font(.caption).foregroundStyle(.secondary)
            HStack(alignment: .top, spacing: 24) {
              field("默认模型") {
                Picker("默认模型", selection: $draft.model) {
                  Text("跟随全局默认").tag("")
                  ForEach(store.profiles) { Text($0.name).tag($0.id) }
                }.labelsHidden().accessibilityIdentifier("project.defaultModel")
              }
              if draft.revision > 0 { field("工具策略") {
                Picker("工具策略", selection: $draft.policy) {
                  Text("编辑和命令 · 按操作授权").tag("ask")
                  Text("只读文件工具").tag("readOnly")
                  Text("不使用工具").tag("noTools")
                }.labelsHidden().accessibilityIdentifier("project.policy")
              } }
            }
            field("项目指令\(draft.revision == 0 ? "（可选）" : "")") {
              TextEditor(text: $draft.instructions).font(.body).frame(
                minHeight: 110, idealHeight: 160
              )
              .padding(8).background(
                Color(nsColor: .textBackgroundColor), in: RoundedRectangle(cornerRadius: 10)
              )
              .accessibilityIdentifier("project.instructions")
            }
            Text(draft.revision == 0 ? "只建立关联；不会复制、上传或修改文件夹内容。" : "从下一回合生效。不同默认目录用于新聊天，已有聊天保留各自工作区。")
              .font(.caption).foregroundStyle(.secondary)
          }.textFieldStyle(.roundedBorder).controlSize(.large).padding(.vertical, 8)
        }
      } else if section == "Skills" {
        ScrollView { SkillsView(scope: "project", scopeID: draft.id) }
      } else {
        VStack(alignment: .leading, spacing: 22) {
          GroupBox("文件协同") {
            Text("项目聊天共用这个文件夹。受控文件修改在写入前检查原文件版本；遇到外部修改会拒绝覆盖，保留冲突信息。")
              .frame(maxWidth: .infinity, alignment: .leading).padding(16)
          }
          GroupBox("命令与授权") {
            VStack(alignment: .leading, spacing: 14) {
              Text("工具策略由本项目设置决定。修改文件和执行命令仍需对应操作的授权；Skills 不能自行扩大权限。")
              Button("查看授权记录") {
                store.settingsSection = "授权记录"
                openSettings()
              }
            }.frame(maxWidth: .infinity, alignment: .leading).padding(16)
          }
          Spacer()
        }
      }
      if !store.projectEditorError.isEmpty {
        Text(store.projectEditorError).foregroundStyle(.red).textSelection(.enabled)
          .accessibilityIdentifier("project.error")
      }
      HStack {
        if draft.revision > 0 { Button("移除项目…", role: .destructive) { store.removeProject(draft) } }
        Spacer()
        Button("取消", action: close)
        Button(draft.revision == 0 ? "创建项目" : "保存更改") {
          guard ownerEpoch == store.identityEpoch else { return }
          draft.name = draft.name.trimmingCharacters(in: .whitespacesAndNewlines)
          store.saveProject(draft, relink: relink)
        }.buttonStyle(.borderedProminent).disabled(draft.name.isEmpty || draft.path.isEmpty)
          .accessibilityIdentifier("project.save")
      }
      if embedded {
        Button("固定资料：\(draft.context.count) 份  →") {
          close()
          store.projectPage = "context"
          if let current = store.currentProject { store.reloadProjectReferences(current) }
        }.buttonStyle(.plain).foregroundStyle(.secondary)
      }
    }
  }
  private func field<Content: View>(_ title: String, @ViewBuilder content: () -> Content)
    -> some View
  {
    VStack(alignment: .leading, spacing: 10) {
      Text(title).font(.callout).foregroundStyle(.secondary)
      content().frame(maxWidth: .infinity, alignment: .leading)
    }.frame(maxWidth: .infinity, alignment: .leading)
  }
  private func close() {
    if embedded {
      guard ownerEpoch == store.identityEpoch else { return }
      store.editingProject = nil
      store.route = "project"
    } else {
      dismiss()
    }
  }
  private func selectFolder() {
    let panel = NSOpenPanel()
    panel.canChooseFiles = false
    panel.canChooseDirectories = true
    panel.canCreateDirectories = true
    panel.prompt = "选择项目文件夹"
    guard panel.runModal() == .OK, let url = panel.url?.resolvingSymlinksInPath() else { return }
    draft.path = url.path
    relink = true
    if draft.name.isEmpty { draft.name = url.lastPathComponent }
    draft.bookmark = (try? url.bookmarkData(options: .minimalBookmark).base64EncodedString()) ?? ""
  }
}
struct ArchivedChatsView: View {
  @EnvironmentObject var store: WorkspaceStore
  var body: some View {
    VStack(alignment: .leading, spacing: 24) {
      Text("已归档").font(.system(size: 27, weight: .semibold))
      Text("归档保留完整聊天记录。可以恢复，也可以永久删除并清理缓存。").foregroundStyle(.secondary)
      List(store.chats.filter { $0.archived }) { chat in
        HStack {
          Text(chat.title)
          Spacer()
          Button("恢复") { store.updateChat(chat, field: "archived", value: false) }
          Button("永久删除…", role: .destructive) { store.removeChat(chat) }
        }.padding(.vertical, 10)
      }.overlay {
        if !store.chats.contains(where: { $0.archived }) {
          ContentUnavailableView("没有已归档聊天", systemImage: "archivebox")
        }
      }
    }.padding(32)
  }
}
struct ProjectDocumentView: View {
  let document: ProjectDocument
  @Environment(\.dismiss) private var dismiss
  var body: some View {
    VStack(alignment: .leading, spacing: 18) {
      HStack {
        Label(document.path, systemImage: "doc.text").font(.headline)
        Spacer()
        Button("完成") { dismiss() }
      }
      ScrollView([.horizontal, .vertical]) {
        Text(document.text).font(.system(size: 13, design: .monospaced)).textSelection(.enabled)
          .frame(maxWidth: .infinity, alignment: .topLeading).padding()
      }
      Text("SHA-256 · \(document.sha256)").font(.caption2.monospaced()).foregroundStyle(.secondary)
        .textSelection(.enabled)
    }.padding(24).frame(width: 820, height: 600)
  }
}
