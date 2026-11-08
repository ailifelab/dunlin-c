import SwiftUI
struct InstalledTheme:Decodable,Identifiable {let id:String;let name:String;let scheme:String;let accent:String;let tokens:[String:String]?}
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
  var themeText:Color {themeColor("text",fallback:.primary)}
  var themeTint:Color {
    guard let value=installedThemes.first(where:{$0.id==theme})?.accent,value.count==7,let rgb=UInt32(value.dropFirst(),radix:16) else{return .accentColor}
    return Color(red:Double((rgb>>16)&255)/255,green:Double((rgb>>8)&255)/255,blue:Double(rgb&255)/255)
  }
  func refreshThemes(){refreshPluginPages();extensionRequest(["op":"extensions.themes"],as:[InstalledTheme].self){r in if r.ok{self.installedThemes=r.data ?? []}}}
}

// v5.1 uses a broad, quiet sidebar tint; no edge strip or seam shadow.
struct SidebarBackdrop:View {
  @EnvironmentObject var store:WorkspaceStore
  @Environment(\.colorScheme) private var colorScheme
  private func rgb(_ value:UInt32)->Color {
    Color(red:Double((value>>16)&255)/255,green:Double((value>>8)&255)/255,blue:Double(value&255)/255)
  }
  var body:some View {
    if store.installedThemes.contains(where:{$0.id==store.theme}) {
      store.themeSurface
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
