# R8 规则（release 上架包用）
#
# 本项目没有反射、没有 JNI、没有 GSON/Jackson 之类的注解解析，
# Compose 与 AndroidX 的 keep 规则由各自 AAR 自带的 consumer-rules 提供，
# 所以这里只做两件事：保留平台 org.json 的告警噪音、保留必要的类属性。
#
# 改这里的判据：只在 release 包出现 ClassNotFoundException / NoSuchMethodError /
# 字段变 null 时才加规则，并且加完要复跑「双机收发 + 看图 + 分享」冒烟。

# 平台 org.json 在编译期可见、运行期由 bootclasspath 提供，R8 会报缺类告警
-dontwarn org.json.**

# 泛型签名与内部类信息：JSON 手工拼串不依赖它们，但保留成本几乎为零，
# 万一以后接反射/序列化不用回来补
-keepattributes Signature,InnerClasses,EnclosingMethod,Exceptions

# 保留行号：线上崩溃栈要能对回源码（体积代价可忽略）
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile
