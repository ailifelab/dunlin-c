import Foundation
import CDesktop

struct NativeModelHelper:Decodable {
  let id:String
  let model:String
  let revision:Int64
}
extension WorkspaceStore {
  // Only C-eligible helpers from an explicit original workbench action enter.
  // The Node host never receives profile JSON, Keychain references or secrets.
  func startNativeModelHelpers() {
    guard pendingIdentitySwitch == nil,!terminating,!helperReading,configurationReady,!switchingModel,!configSaving,
      !checkingConnection,handle != nil,(!busy || !agentWorkers.isEmpty || !helperWorkers.isEmpty) else{return}
    helperReading=true
    let epoch=identityEpoch
    readLocalView(["op":"helpers.pending"],as:[NativeModelHelper].self){result in
      guard self.identityEpoch==epoch else{return}
      self.helperReading=false
      guard self.pendingIdentitySwitch == nil,!self.terminating,result.ok,let jobs=result.data,self.configurationReady,
        !self.switchingModel,!self.configSaving,!self.checkingConnection else{return}
      for job in jobs where self.helperWorkers[job.id]==nil {
        if self.helperWorkers.count+self.agentWorkers.count>=2 {break}
        guard job.revision==self.configRevision,
          let profile=self.profiles.first(where:{$0.id==job.model}),let owner=self.handle else{continue}
        let callbacks=Callbacks()
        self.helperWorkers[job.id]=callbacks;self.busy=true
        self.queue.async {
          guard let raw=dc_open_agent_worker(owner.raw) else {
            _ = dc_helper_cancel(owner.raw,job.id,job.revision)
            DispatchQueue.main.async{self.finishNativeHelper(job.id,epoch:epoch)};return
          }
          let worker=NativeStoreReference(raw:raw)
          DispatchQueue.global(qos:.userInitiated).async {
            if case .success(let secret)=modelCredential(profile) {
              var hooks=dc_hooks(user:Unmanaged.passUnretained(callbacks).toOpaque(),event:nil,approve:nil,cancelled:{raw in
                guard let raw else{return 1}
                return Unmanaged<Callbacks>.fromOpaque(raw).takeUnretainedValue().isCancelled() ? 1:0
              })
              if let result=dc_helper_execute(worker.raw,job.id,job.revision,secret,&hooks){dc_free(result)}
            } else {_ = dc_helper_cancel(worker.raw,job.id,job.revision)}
            DispatchQueue.main.async{self.finishNativeHelper(job.id,epoch:epoch)}
          }
        }
      }
    }
  }
  private func finishNativeHelper(_ id:String,epoch:Int64) {
    guard identityEpoch==epoch else{return}
    helperWorkers.removeValue(forKey:id)
    if helperWorkers.isEmpty && agentWorkers.isEmpty && !agentQueueRunning {
      busy=false;stopping=false
    }
    startNativeModelHelpers()
    startNativePluginJobs()
  }
}
