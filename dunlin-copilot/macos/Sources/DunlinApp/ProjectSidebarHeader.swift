import AppKit
import SwiftUI

// Native buttons preserve three distinct accessibility targets inside a macOS
// sidebar row; a SwiftUI DisclosureGroup label merges its nested buttons.
struct ProjectSidebarHeader: NSViewRepresentable {
  @Environment(\.isEnabled) private var enabled
  let project: ProjectRecord
  let expand: () -> Void
  let properties: () -> Void
  let open: () -> Void
  func makeCoordinator() -> Coordinator { Coordinator(self) }
  func makeNSView(context: Context) -> NSStackView {
    let stack = NSStackView()
    stack.orientation = .horizontal
    stack.alignment = .centerY
    stack.spacing = 8
    stack.setAccessibilityElement(true)
    stack.setAccessibilityRole(.group)
    for (index, action) in [#selector(Coordinator.expandProject), #selector(Coordinator.editProject), #selector(Coordinator.openProject)].enumerated() {
      let button = NSButton(title: "", target: context.coordinator, action: action)
      button.isBordered = false
      button.setButtonType(.momentaryPushIn)
      button.setAccessibilityElement(true)
      button.setAccessibilityRole(.button)
      if index < 2 { button.widthAnchor.constraint(equalToConstant: index == 0 ? 10 : 18).isActive = true }
      else {
        button.alignment = .left
        button.lineBreakMode = .byTruncatingTail
        button.font = .systemFont(ofSize: 13, weight: .medium)
        button.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        button.setContentHuggingPriority(.defaultLow, for: .horizontal)
      }
      stack.addArrangedSubview(button)
    }
    stack.setAccessibilityChildren(stack.arrangedSubviews)
    updateNSView(stack, context: context)
    return stack
  }
  func updateNSView(_ stack: NSStackView, context: Context) {
    context.coordinator.parent = self
    let buttons = stack.arrangedSubviews.compactMap { $0 as? NSButton }
    guard buttons.count == 3 else { return }
    for button in buttons { button.isEnabled = enabled }
    buttons[0].image = NSImage(systemSymbolName: project.expanded ? "chevron.down" : "chevron.right", accessibilityDescription: nil)
    buttons[0].symbolConfiguration = .init(pointSize: 10, weight: .semibold)
    buttons[0].contentTintColor = .secondaryLabelColor
    buttons[0].setAccessibilityLabel((project.expanded ? "折叠项目 " : "展开项目 ") + project.name)
    buttons[0].setAccessibilityIdentifier("project.disclosure." + project.id)
    buttons[1].image = NSImage(systemSymbolName: project.available ? "folder" : "folder.badge.questionmark", accessibilityDescription: nil)
    buttons[1].symbolConfiguration = .init(pointSize: 13, weight: .regular)
    buttons[1].contentTintColor = .secondaryLabelColor
    buttons[1].toolTip = "项目属性"
    buttons[1].setAccessibilityLabel("项目属性：" + project.name)
    buttons[1].setAccessibilityIdentifier("project.settings." + project.id)
    buttons[2].title = project.name
    buttons[2].setAccessibilityLabel("项目主页：" + project.name)
    buttons[2].setAccessibilityIdentifier("project.home." + project.id)
    stack.setAccessibilityLabel(project.name)
    stack.toolTip = project.path
  }
  final class Coordinator: NSObject {
    var parent: ProjectSidebarHeader
    init(_ parent: ProjectSidebarHeader) { self.parent = parent }
    @objc func expandProject() { parent.expand() }
    @objc func editProject() { parent.properties() }
    @objc func openProject() { parent.open() }
  }
}
