import SwiftUI
import AppKit
import CDesktop

@main struct AgentReferenceCheck {
    @MainActor static func main() async {
        let app=NSApplication.shared
        app.setActivationPolicy(.accessory);app.finishLaunching()
        let s=WorkspaceStore()
        func waitFor(_ label:String,_ check:()->Bool) async {
            for _ in 0..<240 {
                if check(){return}
                try? await Task.sleep(for:.milliseconds(80))
            }
            FileHandle.standardError.write(Data("Failed at \(label); \(s.error); \(s.agentError)\n".utf8))
            exit(3)
        }
        await waitFor("startup"){s.configurationReady && !s.creatingChat && s.projects.contains(where:{$0.id=="project"}) && s.chats.contains(where:{$0.id=="direct"})}
        func run(_ chat:String, _ prompt:String,_ withReference:Bool=false) async {
            let old=s.selected
            guard s.openChat(chat) else {fatalError("cannot open \(chat)")}
            s.selectChat(from:old,to:chat)
            await waitFor("model/attachment"){s.profileID=="fixture" && s.attachmentsLoaded && !s.busy}
            if withReference {
                s.selectProjectReference(s.currentProject!,path:"reference.txt",enabled:true)
                await waitFor("select reference"){s.selectedProjectReferences["reference.txt"] != nil}
            }
            s.run(input:prompt)
            await waitFor("settle \(prompt)"){!s.busy && s.messages.contains(where:{$0.content=="Reply \(prompt)"})}
        }
        await run("ordinary","ordinary")
        await run("direct","no-selection")
        await run("direct","selected",true)
        s.startAgentQueue(project:"project")
        await waitFor("worker"){!s.busy && s.agentJobs.contains(where:{$0.id=="worker" && $0.state=="succeeded"})}
        print("PASS native macOS ordinary/project/selected chat and scheduled worker all completed")
        exit(0)
    }
}
