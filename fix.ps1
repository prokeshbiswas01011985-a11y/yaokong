$p='d:\content\软件固件\固件设计规范.md'; $t=[IO.File]::ReadAllText($p,[Text.Encoding]::UTF8)
$t=$t.Replace('接收端 STM32F103C8T6 + NRF24L01，TIM2 输出 4 路 PWM 接飞控','**接收端即飞控本身**：STM32F103C8T6 直连 NRF24L01（SPI），通道数据直接供飞控使用，无独立 NRF转PWM 接收板；飞控 TIM PWM 为电机输出，非遥控输入')
$t=$t.Replace('接收端固件（STM32F103C8T6，TIM2 输出 50Hz 舵机 PWM）','接收端固件首版 ⚠️ 架构理解有误（曾按 NRF转舵机PWM接飞控 编写），待飞控代码到位后重构为飞控内 NRF 接收模块')
[IO.File]::WriteAllText($p,$t,[System.Text.UTF8Encoding]::new($false)); Get-Item $p | Select-Object Length,LastWriteTime
