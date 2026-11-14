"""Extract bounded verbatim passages from the beginning and end of older turns."""
def compress(arguments):
    messages = arguments["messages"]
    passages = []
    remaining = 768
    for message in reversed(messages):
        text = message["content"].strip()
        if not text or remaining < 24:
            continue
        size = min(192, remaining)
        excerpt = text if len(text) <= size else text[:size // 2] + " … " + text[-(size // 2 - 3):]
        passages.append(message["role"] + " · " + excerpt)
        remaining -= len(excerpt)
    return {"summary": "\n".join(reversed(passages))}


def activate(api, config):
    api.register_command("compress_context", compress)
