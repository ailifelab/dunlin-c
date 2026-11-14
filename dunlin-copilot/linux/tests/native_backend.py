#!/usr/bin/env python3
"""Exercise the running GTK client through AT-SPI on a selected X11 or Wayland backend."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time

import pyatspi


def visible(node):
    return node.getState().contains(pyatspi.STATE_SHOWING)


def walk(node):
    pending = [node]
    while pending:
        item = pending.pop()
        try:
            yield item
            pending.extend(item.getChildAtIndex(i) for i in range(item.childCount - 1, -1, -1))
        except RuntimeError:
            continue


def wait_for(predicate, process, label):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError('Client exited while waiting for ' + label)
        try:
            found = predicate()
            if found is not None:
                return found
        except RuntimeError:
            pass
        time.sleep(0.1)
    raise RuntimeError('Timed out waiting for ' + label)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    args = parser.parse_args()
    backend = os.environ.get('GDK_BACKEND', '')
    if backend not in ('x11', 'wayland'):
        parser.error('select exactly one backend with GDK_BACKEND=x11 or wayland')
    app = args.executable.resolve()
    with tempfile.TemporaryDirectory(prefix='dunlin-desktop-ui-') as tmp:
        private = Path(tmp)
        env = dict(os.environ, HOME=tmp, XDG_CONFIG_HOME=str(private / 'config'),
                   XDG_CACHE_HOME=str(private / 'cache'), XDG_DATA_HOME=str(private / 'data'),
                   NO_AT_BRIDGE='0', GTK_MODULES='gail:atk-bridge')
        process = subprocess.Popen([str(app), '--data-dir', str(private / 'identity')], env=env)
        try:
            def window():
                for application in pyatspi.Registry.getDesktop(0):
                    try:
                        if application.get_process_id() != process.pid:
                            continue
                    except AttributeError:
                        # Older at-spi exposes the same application on its own
                        # D-Bus test session, without the pid convenience method.
                        pass
                    for child in application:
                        if child.name == 'Dunlin Copilot' and visible(child):
                            return child
                return None
            frame = wait_for(window, process, 'Dunlin Copilot')
            markers = {
                '项目': '添加项目…', '工具记录': '工具执行记录', '协作与轨迹': '创建任务',
                '权限': '关联远端账号…', '权限与数据': '本机存储', '记忆与知识库': '记忆、知识与检索',
                '插件': '插件与商店', '设置': '添加服务商', '聊天': '发送',
            }
            for menu, marker in markers.items():
                def entry():
                    for item in walk(frame):
                        if item.name == menu and visible(item):
                            try:
                                if item.queryAction().nActions > 0:
                                    return item
                            except NotImplementedError:
                                continue
                    return None
                widget = wait_for(entry, process, menu)
                def detail():
                    # A page-specific heading or another visible control must
                    # appear after navigating. Sidebar labels alone do not count.
                    return next((item for item in walk(frame)
                                 if visible(item) and item.name == marker), None)
                if not widget.queryAction().doAction(0):
                    raise RuntimeError('Navigation action failed: ' + menu)
                time.sleep(.2)
                body = wait_for(detail, process, menu + ' content')
                print(backend, menu, 'accessible native content', body.name)
            print('PASS', backend, 'native GTK window and nine distinct page contents')
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == '__main__':
    main()
