import Foundation

func configDiagnosticMessage(_ diagnostic: ConfigDiagnostic) -> String {
  let messages: [String: String] = [
    "static_syntax": "配置语法有误，请检查缩进、引号与括号。",
    "static_limit": "配置超过大小、层级、节点数量或数字范围限制。",
    "static_tag": "不支持 YAML 显式标签，请移除 !!js 等标签后重新导入。",
    "static_alias": "不支持锚点、引用或合并字段，请展开成静态数据。",
    "static_duplicate": "配置中有重复字段，请保留唯一字段。",
    "static_key": "配置字段名必须是普通字符串。",
    "static_document": "只接受单个 YAML 1.2 兼容文档。",
    "static_root": "配置顶层必须是对象或受支持的 DSH 插件列表。",
    "static_encoding": "请使用 UTF-8，内容中不能含有空字节。",
    "static_memory": "本地解析资源不足，原配置保留。",
    "explicit_models_required": "需要显式 models 清单。当前不从未知内置目录展开 modelOverrides。",
    "explicit_providers_required": "未找到显式的服务商清单。",
    "explicit_protocol_required": "请填写受支持的 api 协议，不会猜测目录中的默认协议。",
    "explicit_endpoint_required": "请填写显式 baseURL，不会自动采用目录默认地址。",
    "unsupported_field": "这个字段暂不支持映射，请移除后重试或在本应用中单独配置。",
    "dynamic_include_not_supported": "不执行动态 include，请先展开成静态配置。",
    "object_required": "此处需要配置对象。",
    "invalid_route": "路由 ID 仅支持 1–64 位字母、数字、连字符和下划线。",
    "invalid_name": "请填写有效的显示名称。",
    "invalid_credential_reference": "凭据引用格式无效；不会从环境变量复制密钥。",
    "duplicate_provider_route": "配置中出现重复的服务商路由。",
    "duplicate_model_id": "同一服务商的模型 ID 不能重复。",
    "explicit_model_id_required": "每个模型都需要显式的字符串 ID。",
    "model_protocol_override_not_supported": "请将不同协议的模型拆分为独立服务商。",
    "invalid_capability": "模型能力字段的类型或取值无效。",
    "unsupported_modality": "当前模型路由仅映射文本与图片输入能力。",
    "text_modality_required": "聊天模型需要声明文本输入能力。",
    "invalid_output_limit": "最大输出上限无效。",
    "invalid_context_window": "模型上下文窗口无效。",
    "configuration_limit": "导入清单超过服务商或模型数量上限。",
    "plugin_id_required": "DSH 静态列表需要显式插件 ID。",
    "invalid_disabled_flag": "disabled 必须是布尔值。",
    "multiple_provider_blocks_not_supported": "请先把多个 llm-pi-ai 块合并为一个显式 providers 清单。",
  ]
  var message = messages[diagnostic.code] ?? "配置字段无法映射，请核对后重试。"
  if let path = diagnostic.path { message += " 位置：\(path)" }
  if let line = diagnostic.line, line > 0 {
    message += " 第 \(line) 行，第 \(diagnostic.column ?? 1) 列。"
  }
  return message
}
func configImportWarning(_ code: String) -> String {
  switch code {
  case "credential_value_omitted", "Credential value omitted; enter it separately in Keychain.":
    return "文件中的密钥值不会导入，请稍后在钥匙串中单独配置。"
  case "imported_routes_are_new": return "所有路由均使用新身份，现有聊天与凭据保持原关联。"
  case "provider_alias_renamed": return "同名服务商将作为新路由导入，已在预览中添加 imported 后缀。"
  case "default_model_not_replaced": return "不会替换现有默认模型，导入后由你单独选择。"
  case "pricing_not_imported": return "定价数据不导入，也不会据此计算费用。"
  case "plugin_declaration_not_installed": return "其他插件声明未安装；本次只提取模型连接。"
  case "static_dsh_subset_only": return "只导入受支持的静态 DSH 服务商与显式模型，不执行插件或 includes。"
  case "static_runtime_subset_only": return "只导入模型参数，原工作区、会话目录和显示偏好不会导入。"
  case "reasoning_effort is not imported by this static subset.":
    return "reasoning_effort 请求选项暂不导入，请在模型设置中核对。"
  default: return code
  }
}
