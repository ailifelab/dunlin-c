import Foundation

struct GrantTaskOwner {
  let id:String
  let chat:String
  let parent:String
  let active:Bool
}
enum GrantCancellationTarget:Equatable {
  case inactive, ordinary, worker(String), unavailable
  static func resolve(status:String,chat:String,ordinaryChat:String?,workers:Set<String>,tasks:[GrantTaskOwner])->Self {
    guard ["awaiting","issued_once"].contains(status) else{return .inactive}
    // A nested task runs on its root worker. Resolve from this chat only;
    // missing or cyclic ancestry must never fall back to cancelling everything.
    let parents=Dictionary(tasks.map{($0.id,$0.parent)},uniquingKeysWith:{first,_ in first})
    for task in tasks where task.chat==chat && task.active {
      var id=task.id,visited=Set<String>()
      while !id.isEmpty,visited.insert(id).inserted {
        if workers.contains(id){return .worker(id)}
        guard let parent=parents[id] else{break};id=parent
      }
    }
    if workers.isEmpty,ordinaryChat==chat{return .ordinary}
    return .unavailable
  }
}

func grantStatusText(status:String,allowed:Bool)->String {
  switch status {
  case "awaiting":return "等待确认"
  case "interrupted":return "进程中断 · 已失效"
  case "denied":return "已拒绝"
  case "expired":return "单次 · 已失效"
  default:return allowed ? "仅限当前调用":"未授权"
  }
}
