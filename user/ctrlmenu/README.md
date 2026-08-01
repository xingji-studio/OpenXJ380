# 控制中心页面配置

控制台页面由镜像中的 `/system/resources/ctrlmenu.settings` 生成；仓库源文件是
`resources/ctrlmenu.settings`。文件使用 UTF-8 文本，每行一个声明，字段以 `|` 分隔，空行和
以 `#` 开头的行会被忽略。

页面必须从 `page|标识|侧栏标题` 开始。页面内支持：

- `h1|文字`、`h2|文字`、`h3|文字`
- `text|文字`
- `value|左侧文字|右侧值`
- `button|文字|值|按钮文字|action`
- `input|文字|值|按钮文字|action`
- `switch|文字|说明|状态值|action`
- `divider`、`space|像素`
- `dynamic|default_apps`、`dynamic|network_status`

普通文字可直接写在值字段中，也可以写成 `中文^English`，控制中心会按当前语言显示。
动态值以 `@` 开头，目前包含 `@os_version`、
`@system_version`、`@kernel_version`、`@cpu`、`@memory`、`@resolution`、
`@memory_mb`、`@background`、`@current_time`、`@timezone`、`@language` 和
`@wheel_reverse`。
可写设置还需要在 `cm_settings.cpp` 的 action 分派中绑定后端，以免配置文件获得任意系统调用能力。
