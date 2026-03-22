# gbl_root_canoe
root 8e5/8g5的设备，同时让系统认为BL锁定

ABL.efi:不含superfb，建议设备解锁时使用  fastboot由官方提供

ABL_with_superfastboot.efi:含superfb，建议设备锁定时使用 fastboot由superfb提供

ABL 补丁1：让锁定时跳过错误
补丁2,强制向tee报告锁定，验证通过

因为补丁2存在，锁bl非必须，锁的好处是去除解锁提示

HYPER OS等有防回滚的系统不要锁bl，使用ABL.efi
hyperos 解锁不炸tee，所以这个的用途不大，除了0模块可以隐藏BL状态

如果锁bl模式（ABL.efi）
卸载时非oppo系请使用内建fb解锁
oppo系使用深度测试官方bl解锁