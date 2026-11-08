import AppKit
import CDesktop
import SwiftUI

extension WorkspaceStore {
  func invalidateProjectContent() {
    projectContentEpoch = UUID()
    projectReferenceRequestID = UUID()
    selectedProjectReferences = [:]
    referencePreviews = [:]
    projectReferences = []
    projectFiles = []
    fileDirectory = ""
    openedDocument = nil
  }
  func workspaceRequest<T: Decodable>(
    _ body: [String: Any], as type: T.Type,
    completion: @escaping (WorkspaceReply<T>) -> Void
  ) {
    guard let handle,
      let data = try? JSONSerialization.data(withJSONObject: body, options: .sortedKeys),
      let raw = String(data: data, encoding: .utf8)
    else {
      completion(WorkspaceReply(ok: false, error: "本地存储尚未就绪", data: nil))
      return
    }
    let epoch = identityEpoch
    queue.async {
      let result =
        self.decode(dc_workspace_request(handle.raw, raw), as: WorkspaceReply<T>.self)
        ?? WorkspaceReply(ok: false, error: "本地服务返回了无效结果", data: nil)
      DispatchQueue.main.async { if self.identityEpoch == epoch { completion(result) } }
    }
  }
  func workspaceError(_ code: String?) -> String {
    switch code {
    case "duplicate_name_or_directory_or_revision_conflict": return "项目名称或目录已被使用，或属性已更新。请刷新后重试。"
    case "unavailable_or_overlapping_directory": return "目录不存在、不可访问，或与应用数据目录重叠。请选择独立的项目文件夹。"
    case "active_checkpoint": return "项目内仍有未完成的回合，请先在原模型下处理恢复状态，再修改项目。"
    case "directory_identity_changed": return "这个路径指向的目录已发生变化，请重新选择并关联。"
    case "revision_conflict": return "属性已被更新。请重新打开项目属性，再保存。"
    case "invalid_project", "invalid_name": return "请填写有效的项目名称、目录和项目指令。"
    case "project_unavailable": return "项目目录不可用，请重新关联。"
    case "reference_version_conflict": return "资料已变化、引用不匹配或版本无效，请重新预览后确认。"
    case "unsafe_or_unreadable_path": return "文件不可读取：仅支持项目内的普通 UTF-8 文件（最多 64 KiB），不读取链接。"
    default: return code ?? "操作失败"
    }
  }
  func reloadProjects() {
    workspaceRequest(["op": "projects.list"], as: [ProjectRecord].self) { result in
      if result.ok {
        self.projects = result.data ?? []
      } else {
        self.error = self.workspaceError(result.error)
      }
    }
  }
  func browseProjects() {
    guard !busy, !workspaceTransitioning else { return }
    route = "projects"
    searchVisible = false
    reloadProjects()
  }
  func openProject(_ project: ProjectRecord) {
    guard !busy, !workspaceTransitioning else { return }
    if let selected { persistDraft(selected) }
    selectedProject = project.id
    route = "project"
    searchVisible = false
    projectPage = "chats"
    invalidateProjectContent()
    fileDirectory = ""
    projectFiles = []
    openedDocument = nil
    reloadProjects()
    if project.available { listProjectFiles(project.id) }
    reloadProjectReferences(project)
  }
  func editProject(_ project: ProjectRecord) {
    guard !busy, !workspaceTransitioning else { return }
    let canonical = projects.first(where: { $0.id == project.id }) ?? project
    projectEditorError = ""
    editingProject = canonical
    if canonical.revision > 0 {
      selectedProject = canonical.id
      route = "projectSettings"
      searchVisible = false
    }
  }
  func chooseProjectDefaultFolder(_ project: ProjectRecord) {
    guard !busy, !projectSaving, !workspaceTransitioning,
      let canonical = projects.first(where: { $0.id == project.id }) else { return }
    let panel = NSOpenPanel()
    panel.canChooseFiles = false
    panel.canChooseDirectories = true
    panel.canCreateDirectories = true
    panel.prompt = "选择项目默认文件夹"
    panel.message = "不同目录只用于新聊天；已有聊天保留原工作区。同一物理目录的移动会更新对应路径。"
    guard panel.runModal() == .OK, let url = panel.url?.resolvingSymlinksInPath() else { return }
    var next = canonical
    next.path = url.path
    next.bookmark = (try? url.bookmarkData(options: .minimalBookmark).base64EncodedString()) ?? ""
    saveProject(next, relink: true, navigate: false)
  }
  func saveProject(
    _ project: ProjectRecord, relink: Bool = false, navigate: Bool = true,
    versions: [String: String]? = nil
  ) {
    guard !busy, !projectSaving,
      let data = try? JSONEncoder().encode(project),
      var body = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
    else { return }
    body["op"] = project.revision == 0 ? "projects.create" : "projects.update"
    body["relink"] = relink
    if let versions { body["contextVersions"] = versions }
    projectSaving = true
    workspaceRequest(body, as: ProjectRecord.self) { result in
      self.projectSaving = false
      if result.ok, let saved = result.data {
        self.invalidateProjectContent()
        if let index = self.projects.firstIndex(where: { $0.id == saved.id }) {
          self.projects[index] = saved
        }
        if navigate {
          self.editingProject = nil
          self.projectEditorError = ""
          self.selectedProject = saved.id
          self.route = "project"
        }
        self.reloadProjects()
        self.reload()
        if self.currentProject?.id == saved.id { self.reloadProjectReferences(saved) }
        if saved.available && self.currentProject?.id == saved.id {
          self.listProjectFiles(saved.id)
        }
      } else {
        if navigate {
          self.projectEditorError = self.workspaceError(result.error)
        } else {
          self.error = self.workspaceError(result.error)
        }
      }
    }
  }
  func setExpanded(_ project: ProjectRecord, _ value: Bool) {
    if let i = projects.firstIndex(where: { $0.id == project.id }) { projects[i].expanded = value }
    workspaceRequest(
      ["op": "projects.expanded", "id": project.id, "value": value], as: NoData.self
    ) {
      if !$0.ok {
        self.error = self.workspaceError($0.error)
        self.reloadProjects()
      }
    }
  }
  func removeProject(_ project: ProjectRecord) {
    guard !busy else { return }
    let alert = NSAlert()
    alert.messageText = "移除项目“\(project.name)”？"
    alert.informativeText = "项目文件始终保留。可以把聊天保留在最近聊天中，或一并归档。聊天可单独永久删除。"
    alert.addButton(withTitle: "保留聊天并移除")
    alert.addButton(withTitle: "归档聊天并移除")
    alert.addButton(withTitle: "取消")
    alert.buttons[0].keyEquivalent = ""
    alert.window.defaultButtonCell = nil
    let answer = alert.runModal()
    guard answer != .alertThirdButtonReturn else { return }
    workspaceRequest(
      [
        "op": "projects.remove", "id": project.id, "revision": project.revision,
        "mode": answer == .alertFirstButtonReturn ? "keep" : "archive",
      ], as: NoData.self
    ) { result in
      if result.ok {
        self.editingProject = nil
        self.selectedProject = nil
        self.route = "projects"
        self.reloadProjects()
        self.reload()
      } else {
        self.error = self.workspaceError(result.error)
      }
    }
  }
  func updateChat(_ chat: Chat, field: String, value: Any) {
    guard !busy else { return }
    workspaceRequest(
      ["op": "chats.update", "id": chat.id, "field": field, "value": value], as: NoData.self
    ) {
      if $0.ok {
        if field == "archived", let archived = value as? Bool, archived, self.selected == chat.id {
          self.selected = nil
          self.messages = []
          self.route = "archive"
        }
        self.reload()
        self.reloadProjects()
      } else {
        self.error = self.workspaceError($0.error)
      }
    }
  }
  func scheduleDraftSave() {
    draftSave?.cancel()
    guard !busy, route == "chat", let id = selected else { return }
    drafts[id] = draft
    let epoch = identityEpoch
    let task = DispatchWorkItem { [weak self] in
      guard let self, self.identityEpoch == epoch, self.selected == id, !self.busy else { return }
      self.persistDraft(id)
    }
    draftSave = task
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.35, execute: task)
  }
  func persistDraft(_ id: String) {
    guard chats.contains(where: { $0.id == id }) else { return }
    drafts[id] = draft
    workspaceRequest(
      ["op": "chats.update", "id": id, "field": "draft", "value": draft], as: NoData.self
    ) { result in
      if !result.ok { self.error = self.workspaceError(result.error) }
    }
  }
  func restoreChatMetadata(_ id: String) {
    workspaceRequest(["op": "chats.metadata", "id": id], as: ChatMetadata.self) { result in
      guard self.selected == id, self.route == "chat", !self.busy, let value = result.data else { return }
      // Never replace text entered while the C query was in flight.
      if self.draft.isEmpty { self.draft = self.drafts[id] ?? value.draft }
      if !value.model.isEmpty, self.profiles.contains(where: { $0.id == value.model }) {
        self.profileID = value.model
      } else if self.selectedChat?.active == true {
        self.profileID = ""
        self.error = "这个回合的原模型配置不可用，请恢复原配置后继续。"
      } else if let project = self.currentProject,
        self.profiles.contains(where: { $0.id == project.model })
      {
        self.profileID = project.model
      } else {
        self.profileID = self.defaultProfileID
      }
    }
  }
  func listProjectFiles(_ id: String, path: String = "") {
    guard !busy else { return }
    fileDirectory = path
    let scope = projectContentEpoch
    workspaceRequest(projectFileRequest("files.list", project: id, path: path), as: [ProjectFile].self) {
      result in
      guard self.currentProject?.id == id, self.fileDirectory == path,
        self.projectContentEpoch == scope else { return }
      if result.ok {
        self.projectFiles = (result.data ?? []).sorted {
          $0.directory != $1.directory
            ? $0.directory : $0.name.localizedStandardCompare($1.name) == .orderedAscending
        }
      } else {
        self.error = self.workspaceError(result.error)
      }
    }
  }
  func readProjectDocument(
    _ project: ProjectRecord, path: String,
    completion: ((ProjectDocument) -> Void)? = nil
  ) {
    let scope = projectContentEpoch
    workspaceRequest(
      projectFileRequest("files.read", project: project.id, path: path), as: ProjectDocument.self
    ) { result in
      guard self.projectContentEpoch == scope, self.currentProject?.id == project.id,
        self.currentProject?.path == project.path else { return }
      if result.ok, let doc = result.data {
        if self.currentProject?.id == project.id { self.referencePreviews[path] = doc }
        if let completion {
          completion(doc)
        } else if self.currentProject?.id == project.id {
          self.openedDocument = doc
        }
      } else {
        self.error = self.workspaceError(result.error)
      }
    }
  }
  func pinContext(_ project: ProjectRecord, path: String) {
    guard !busy else { return }
    readProjectDocument(project, path: path) { doc in
      guard self.currentProject?.id == project.id else { return }
      var next = project
      if !next.context.contains(path) {
        next.context.append(path)
        self.saveProject(next, navigate: false, versions: [path: doc.sha256])
      }
    }
  }
  func unpinContext(_ project: ProjectRecord, path: String) {
    selectedProjectReferences.removeValue(forKey: path)
    var next = project
    next.context.removeAll { $0 == path }
    saveProject(next, navigate: false)
  }
  func reloadProjectReferences(_ project: ProjectRecord) {
    let requestID = UUID()
    projectReferenceRequestID = requestID
    workspaceRequest(
      projectFileRequest("files.references", project: project.id), as: [ProjectReferenceState].self
    ) { result in
      guard self.currentProject?.id == project.id, self.projectReferenceRequestID == requestID
      else { return }
      if result.ok {
        self.projectReferences = result.data ?? []
      } else {
        self.error = self.workspaceError(result.error)
      }
    }
  }
  private func projectFileRequest(_ op: String, project: String, path: String? = nil) -> [String: Any] {
    var body: [String: Any] = ["op": op, "project": project]
    if let path { body["path"] = path }
    if route == "chat", let chat = selectedChat, chat.project == project { body["chat"] = chat.id }
    return body
  }
  func selectProjectReference(_ project: ProjectRecord, path: String, enabled: Bool) {
    guard !busy else { return }
    if !enabled {
      selectedProjectReferences.removeValue(forKey: path)
      return
    }
    let chat = selected
    let epoch = projectReferenceRequestID
    readProjectDocument(project, path: path) { doc in
      guard self.currentProject?.id == project.id, self.selected == chat,
        self.projectReferenceRequestID == epoch
      else { return }
      self.selectedProjectReferences[path] = doc.sha256
    }
  }
  func acknowledgeProjectReference(_ project: ProjectRecord, path: String) {
    guard !busy, let doc = referencePreviews[path],
      let row = projectReferences.first(where: { $0.path == path }), row.currentSHA == doc.sha256
    else {
      error = "请先预览当前文件版本。"
      return
    }
    saveProject(project, navigate: false, versions: [path: doc.sha256])
  }
}
