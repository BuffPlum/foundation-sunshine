# BuffPlum Fork Security Notice / 安全说明

## 中文

这是由 BuffPlum 独立维护的非官方 Foundation Sunshine。AlkaidLab、LizardByte、Moonlight 以及相关上游项目不为本 Fork 的专属功能提供支持。

### 全盘文件传输的信任边界

- 已配对并通过认证的客户端可以列出 Sunshine 进程能够访问的全部磁盘，并在可写位置上传文件。
- HTTPS 能力接口、客户端配对身份和短期 WebSocket 令牌用于限制访问，但不能代替可信网络边界。
- 仅应在个人设备和可信局域网中使用。不要把 Sunshine、GameStream 或文件传输端口直接映射到公网。
- 使用完成串流所需的最低权限运行 Sunshine。管理员、SYSTEM 或服务账号权限会扩大客户端可见范围。
- Windows 服务账号可能看不到交互用户的网络盘，也可能访问交互用户无法访问的其他位置；启用前应实际核对运行身份。
- 默认拒绝覆盖同名文件，临时文件也会在提交前使用独立名称，但任何远程写入功能都不能替代离线备份。
- GitHub Actions 生成的安装包目前不包含商业代码签名。请仅从 [BuffPlum/foundation-sunshine Releases](https://github.com/BuffPlum/foundation-sunshine/releases) 下载并核对 `SHA256SUMS.txt`。

上游当前因安全边界、测试和长期维护成本决定不合并或维护该功能。Fork 专属安全问题请提交到 [BuffPlum/foundation-sunshine Issues](https://github.com/BuffPlum/foundation-sunshine/issues)，不要发送密码、令牌、证书、个人文件或未脱敏日志。

## English

This is an unofficial Foundation Sunshine version independently maintained by BuffPlum. The AlkaidLab, LizardByte, and Moonlight upstream projects do not support fork-specific features.

An authenticated paired client can enumerate every drive readable by the Sunshine process and upload into writable locations. Pairing, HTTPS capability checks, and short-lived WebSocket tokens reduce exposure but do not replace a trusted network boundary. Use the feature only between personal devices on a trusted LAN, never expose its ports directly to the public Internet, run Sunshine with the least privileges needed, and keep independent offline backups.

GitHub Actions packages are currently unsigned. Download them only from the BuffPlum release page and verify `SHA256SUMS.txt`. Report fork-specific security problems through [BuffPlum/foundation-sunshine Issues](https://github.com/BuffPlum/foundation-sunshine/issues) without attaching credentials, certificates, personal files, or unredacted sensitive logs.
