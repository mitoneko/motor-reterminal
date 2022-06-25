# Led-PWM-Reterminal
ReterminalによるGPIOのPWM制御(GPIO18使用)

Reterminalにおいて、GPIO18をPWM制御するためのカーネルモジュールのソースです。
使用するためには、カーネルモジュールのビルド環境を整えた上で、makeする必要があります。
dtbsに関しては、次のコマンドで、コンパイルできます。

```
$dtc -@ -I dts -O dtb -o extled.dtbo extled.dtso
```

コンパイルができれば、生成された、extled.ko extled.dtboを適当なディレクトリに配置し、

```
$ sudo mkdir /sys/kernel/config/device-tree/overlays/extled
$ sudo cp beep.dtbo /sys/kernel/config/device-tree/overlays/extled/dtbo
```

で、デバイスツリーを認識させ、

```
$sudo insmod extled.ko
```

で組み込めます。

```
$echo 5 | sudo tee /dev/extled0
```

で、GPIO18に接続したLEDを点灯できればOKです。5を一桁の適当な数字に変えれば明るさが変わります。

# ソースの2次使用について
　カーネルモジュールのサンプルとして、ご自由にお使いください。ライセンス条件は、GPLとします。
