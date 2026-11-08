import Foundation
@main struct GrantCancellationChecks {
 static func main(){
  let tasks=[GrantTaskOwner(id:"a",chat:"a-chat",parent:"",active:true),GrantTaskOwner(id:"b",chat:"b-chat",parent:"",active:true),GrantTaskOwner(id:"child",chat:"child-chat",parent:"a",active:true)]
  func target(_ status:String,_ chat:String,_ workers:Set<String>=["a","b"],_ ordinary:String?=nil)->GrantCancellationTarget {
   .resolve(status:status,chat:chat,ordinaryChat:ordinary,workers:workers,tasks:tasks)
  }
  for status in ["expired","denied","interrupted"] {precondition(target(status,"a-chat") == .inactive)}
  precondition(target("issued_once","a-chat") == .worker("a"))
  precondition(target("awaiting","child-chat") == .worker("a"))
  precondition(target("issued_once","b-chat") == .worker("b"))
  precondition(target("issued_once","other") == .unavailable)
  precondition(target("issued_once","normal",[],"normal") == .ordinary)
  precondition(target("issued_once","other",[],"normal") == .unavailable)
  let cycle=[GrantTaskOwner(id:"x",chat:"c",parent:"y",active:true),GrantTaskOwner(id:"y",chat:"d",parent:"x",active:true)]
  precondition(GrantCancellationTarget.resolve(status:"awaiting",chat:"c",ordinaryChat:nil,workers:["a"],tasks:cycle) == .unavailable)
  precondition(grantStatusText(status:"awaiting",allowed:false)=="等待确认")
  precondition(grantStatusText(status:"denied",allowed:false)=="已拒绝")
  precondition(grantStatusText(status:"expired",allowed:true)=="单次 · 已失效")
  print("PASS inactive inspection never cancels; active grants target their worker or nested owner; unrelated/missing/cyclic ownership fails closed")
 }
}
