import SwiftUI
struct InstalledTheme:Decodable,Identifiable {let id:String;let name:String;let scheme:String;let accent:String;let tokens:[String:String]?;let preference:String?;let webTokens:[String:String]?}
struct PluginPresentation:Codable {
  struct Glass:Codable {let enabled:Bool,mode:String,blur:Double,frost:Double,brightness:Double}
  let owner:String,preferences:[String],selectedCSS:String,globalCSS:String,glow:Bool,glass:Glass
}
extension WorkspaceStore {
  var themeScheme:ColorScheme? {
    if theme=="dark" {return .dark};if theme=="light" {return .light}
    guard let item=installedThemes.first(where:{$0.id==theme}) else{return nil}
    return item.scheme=="dark" ? .dark:.light
  }
  func themeColor(_ key:String,fallback:Color)->Color {
    guard let value=installedThemes.first(where:{$0.id==theme})?.tokens?[key],value.count==7,let rgb=UInt32(value.dropFirst(),radix:16) else{return fallback}
    return Color(red:Double((rgb>>16)&255)/255,green:Double((rgb>>8)&255)/255,blue:Double(rgb&255)/255)
  }
  private func adaptiveColor(light:UInt32,dark:UInt32)->Color {
    Color(nsColor:NSColor(name:nil) {appearance in
      let value=appearance.bestMatch(from:[.darkAqua,.aqua]) == .darkAqua ? dark:light
      return NSColor(srgbRed:Double((value>>16)&255)/255,green:Double((value>>8)&255)/255,blue:Double(value&255)/255,alpha:1)
    })
  }
  var themeBackground:Color {themeColor("background",fallback:adaptiveColor(light:0xF7F8FA,dark:0x1B1D21))}
  var themeConversationBackground:Color {themeColor("background",fallback:adaptiveColor(light:0xFFFFFF,dark:0x101113))}
  var themeSurface:Color {themeColor("surface",fallback:adaptiveColor(light:0xFFFFFF,dark:0x2A2E35))}
  var themeHTMLPalette:[String:String] {
    guard let selected=installedThemes.first(where:{$0.id==theme}) else{return [:]}
    var tokens=selected.tokens ?? [:];tokens["accent"]=selected.accent;return tokens
  }
  var pluginThemeDefinition:JSONValue? {
    guard let theme=installedThemes.first(where:{$0.id==self.theme}),let tokens=theme.webTokens,!tokens.isEmpty else{return nil}
    return .object(["id":.string(theme.preference ?? theme.id),"name":.string(theme.name),"colorScheme":.string(theme.scheme),"tokens":.object(tokens.mapValues{.string($0)}),"owner":.string(theme.id.split(separator:":").dropFirst().first.map(String.init) ?? "")])
  }
  var themeText:Color {themeColor("text",fallback:.primary)}
  var themeTint:Color {
    guard let value=installedThemes.first(where:{$0.id==theme})?.accent,value.count==7,let rgb=UInt32(value.dropFirst(),radix:16) else{return .accentColor}
    return Color(red:Double((rgb>>16)&255)/255,green:Double((rgb>>8)&255)/255,blue:Double(rgb&255)/255)
  }
  var selectedPresentation:PluginPresentation? {
    guard let selected=installedThemes.first(where:{$0.id==theme}) else{return nil};return pluginPresentations.first{selected.id.hasPrefix("plugin:\($0.owner):") && $0.preferences.contains(selected.preference ?? "")}
  }
  var activeGlass:PluginPresentation.Glass? {let value=(selectedPresentation ?? pluginPresentations.first)?.glass;return value?.enabled == true ? value:nil}
  var pluginPresentationValues:JSONValue {guard let data=try? JSONEncoder().encode(pluginPresentations),let value=try? JSONDecoder().decode(JSONValue.self,from:data) else{return .array([])};return value}
  func refreshPresentations(){extensionRequest(["op":"extensions.presentation.read"],as:[PluginPresentation].self){r in self.pluginPresentations=r.ok ? (r.data ?? []):[]}}
  func refreshThemes() {
    refreshPresentations()
    refreshPluginPages()
    extensionRequest(["op":"extensions.themes"],as:[InstalledTheme].self) { result in
      guard result.ok, let themes=result.data else {return}
      let activeWasInstalled=self.installedThemes.contains(where:{$0.id==self.theme})
      self.installedThemes=themes
      if activeWasInstalled && !themes.contains(where:{$0.id==self.theme}) {self.theme="system";self.saveSettings()}
    }
  }
}

// v5.1 uses a broad, quiet sidebar tint; no edge strip or seam shadow.
struct SidebarBackdrop:View {
  @EnvironmentObject var store:WorkspaceStore
  @Environment(\.colorScheme) private var colorScheme
  private func rgb(_ value:UInt32)->Color {
    Color(red:Double((value>>16)&255)/255,green:Double((value>>8)&255)/255,blue:Double(value&255)/255)
  }
  var body:some View {
    if store.activeGlass != nil || store.installedThemes.contains(where:{$0.id==store.theme}) {
      PluginAppearanceBackdrop(sidebar:true)
    } else {
      LinearGradient(stops:colorScheme == .dark ? [
        .init(color:rgb(0x3C4351).opacity(0.94),location:0),
        .init(color:rgb(0x252C37).opacity(0.96),location:0.5),
        .init(color:rgb(0x302D3C).opacity(0.94),location:1)
      ]:[
        .init(color:Color.white.opacity(0.90),location:0),
        .init(color:rgb(0xE4ECF8).opacity(0.84),location:0.5),
        .init(color:rgb(0xE8E3F2).opacity(0.91),location:1)
      ],startPoint:.topLeading,endPoint:.bottomTrailing)
    }
  }
}

// The native mapping consumes typed hints; plugin CSS stays inside WebKit.
struct PluginAppearanceBackdrop:View {
  @EnvironmentObject var store:WorkspaceStore
  var sidebar=false
  var conversation=true
  private var base:Color {sidebar ? store.themeSurface:(conversation ? store.themeConversationBackground:store.themeBackground)}
  var body:some View {
    ZStack {
      if let glass=store.activeGlass {
        Rectangle().fill(glass.mode=="compat" || glass.blur>=8 ? .regularMaterial:.ultraThinMaterial)
        base.opacity(min(0.92,0.22+glass.frost/150))
        if glass.brightness != 50 {(glass.brightness>50 ? Color.white:Color.black).opacity(abs(glass.brightness-50)/200)}
      } else {base}
      if store.selectedPresentation?.glow == true {
        LinearGradient(colors:[store.themeTint.opacity(0.11),.clear,Color.pink.opacity(0.06)],startPoint:.topLeading,endPoint:.bottomTrailing)
      }
    }.allowsHitTesting(false)
  }
}

struct AppearanceSettingsView: View {
  @EnvironmentObject var store: WorkspaceStore
  @State private var selection = "system"
  @State private var browsing = false

  var body: some View {
    VStack(alignment: .leading, spacing: 28) {
      LazyVGrid(columns: [GridItem(.adaptive(minimum: 240), spacing: 18)], spacing: 18) {
        tile("科技白", id: "light", dark: false)
        tile("科技黑", id: "dark", dark: true)
        ForEach(store.installedThemes) { theme in
          tile(theme.name, id: theme.id, dark: theme.scheme == "dark", palette: theme.tokens)
        }
      }
      Toggle("跟随系统", isOn: Binding(get: { selection == "system" }, set: {
        selection = $0 ? "system" : (store.themeScheme == .dark ? "dark" : "light")
      })).toggleStyle(.switch)
      VStack(alignment: .leading, spacing: 10) {
        Text("网页区域专用皮肤").font(.headline)
        Text("仅提供网页样式的插件作用于插件界面。原生侧栏和授权窗口继续使用当前外观。")
          .font(.callout).foregroundStyle(.secondary)
        Text("提供原生颜色的主题可同时用于侧栏和聊天。需要替换整个窗口的扩展将在兼容详情中标注。").font(.callout).foregroundStyle(.secondary)
      }.padding(20).frame(maxWidth: .infinity, alignment: .leading).background(store.themeSurface, in: RoundedRectangle(cornerRadius: 12))
      HStack {
        Button("浏览皮肤商店") { browsing = true }
        Spacer()
        Button("应用所选皮肤") { store.theme = selection }.buttonStyle(.borderedProminent)
          .disabled(selection == store.theme).accessibilityIdentifier("appearance.apply")
      }
      PluginThemeSettings()
    }
    .onAppear { selection = store.theme; store.refreshThemes() }
    .onChange(of: store.theme) { _, value in selection = value }
    .onChange(of: store.installedThemes.map(\.id)) { _, ids in
      if !["light","dark","system"].contains(selection) && !ids.contains(selection) { selection = "system" }
    }
    .sheet(isPresented: $browsing, onDismiss: store.refreshThemes) {
      VStack {
        HStack { Text("皮肤商店").font(.title2); Spacer(); Button("完成") { browsing = false } }
        ScrollView { CatalogView(initialKind: "theme", closeMarketplace: { browsing = false }) }
      }.padding(26).frame(width: 950, height: 680)
    }
  }

  private func tile(_ title: String, id: String, dark: Bool, palette: [String:String]? = nil) -> some View {
    func color(_ token: String, _ fallback: Color) -> Color {
      guard let hex = palette?[token], hex.count == 7, let n = UInt32(hex.dropFirst(), radix: 16) else { return fallback }
      return Color(red: Double(n >> 16) / 255, green: Double((n >> 8) & 255) / 255, blue: Double(n & 255) / 255)
    }
    let ink = color("text", dark ? .white : .black).opacity(0.6)
    return Button { selection = id } label: {
      VStack(alignment: .leading, spacing: 10) {
        HStack(spacing: 10) {
          RoundedRectangle(cornerRadius: 4).fill(color("surface", .gray)).frame(width: 40)
          VStack(alignment: .leading, spacing: 8) {
            Rectangle().fill(ink).frame(height: 2)
            Rectangle().fill(ink).frame(width: 68, height: 2)
            Spacer()
            RoundedRectangle(cornerRadius: 4).strokeBorder(ink).frame(height: 20)
          }.padding(.vertical, 6)
        }.padding(7).frame(height: 92)
          .background(color("background", dark ? Color(white: 0.10) : .white), in: RoundedRectangle(cornerRadius: 7))
          .overlay(RoundedRectangle(cornerRadius: 7).strokeBorder(.secondary.opacity(0.3)))
        Text(title).font(.headline)
        Text(id == "dark" || id == "light" ? "内置 · 全部界面" : "第三方 · 已适配原生颜色").font(.caption).foregroundStyle(.secondary)
      }.padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .background(store.themeSurface, in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(selection == id ? store.themeTint : .secondary.opacity(0.15), lineWidth: selection == id ? 2 : 1))
    }.buttonStyle(.plain).accessibilityIdentifier("appearance.preview." + id)
      .accessibilityAddTraits(selection == id ? .isSelected : [])
  }
}
