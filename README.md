# motor-Reterminal
ReterminalによるGPIOでのモータードライバ

Reterminalにおいて、Hブリッジを利用したモーター制御のためのカーネルモジュールのソースです。
使用するためには、カーネルモジュールのビルド環境を整えた上で、makeする必要があります。
dtbsに関しては、次のコマンドで、コンパイルできます。

```
$dtc -@ -I dts -O dtb -o motor.dtbo motor.dtso
```

コンパイルができれば、生成された、motor.ko motor.dtboを適当なディレクトリに配置し、

```
$ sudo mkdir /sys/kernel/config/device-tree/overlays/motor
$ sudo cp beep.dtbo /sys/kernel/config/device-tree/overlays/motor/dtbo
```

で、デバイスツリーを認識させ、

```
$sudo insmod motor.ko
```

で組み込めます。

```
$echo 5 | sudo tee /dev/motor0
```

で、GPIO18・23に接続したモーターを回転できればOKです。5を一桁の適当な数字に変えれば速度が変わります。正の数値で正転、負の数値で逆転となります。

sysfsにて、PWM制御に使用している、周波数及びpwmの粒度を調整できます。/sys/devices/platform/motor@0配下の、period,max\_pwmに設定する値を数値の文字列で書き込んでください。period(周期)の数値の単位はnsで、max\_pwmの数値はpwmで指定する数値の絶対値の最大値となります。

# ソースの2次使用について
　カーネルモジュールのサンプルとして、ご自由にお使いください。ライセンス条件は、GPLとします。
