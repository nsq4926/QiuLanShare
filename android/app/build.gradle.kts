import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// 上架签名材料不入库：android/keystore.properties 指向 android/keystore/*.jks，
// 两者都在 .gitignore 里。文件不存在时 release 退回未签名（只影响本地试打包）。
val keystorePropsFile = rootProject.file("keystore.properties")
val keystoreProps = Properties().apply {
    if (keystorePropsFile.exists()) keystorePropsFile.inputStream().use { load(it) }
}

android {
    namespace = "com.sanqiu.lanshare"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.sanqiu.lanshare"
        minSdk = 23
        targetSdk = 36
        // 1.0.0 → 10000，以后每发一版 +1（酷安等商店都按 versionCode 判更新）
        versionCode = 10000
        versionName = "1.0.0"
    }

    signingConfigs {
        if (keystorePropsFile.exists()) {
            create("release") {
                storeFile = rootProject.file(keystoreProps.getProperty("storeFile"))
                storePassword = keystoreProps.getProperty("storePassword")
                keyAlias = keystoreProps.getProperty("keyAlias")
                keyPassword = keystoreProps.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        release {
            // 上架包开 R8：包体从 ~20 MB 降到个位数；Compose/AndroidX 的 keep 规则由 AAR 自带
            isMinifyEnabled = true
            // 资源收缩只省几十 KB，但历史上曾怀疑它裁掉 API<26 的传统图标；
            // 实测（2026-11-22）开关两种都保留 48/72/96/144/192 五档 mipmap，故保持开启。
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            if (keystorePropsFile.exists()) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    // 按 ABI 拆分发行包：依赖里的 libandroidx.graphics.path.so 覆盖四个架构，
    // 每个架构单独出一个 APK，另保留一个 universal 包方便直接分发。
    splits {
        abi {
            isEnable = true
            reset()
            include("arm64-v8a", "armeabi-v7a", "x86", "x86_64")
            isUniversalApk = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    packaging {
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2025.12.01"))
    implementation("androidx.activity:activity-compose:1.12.4")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    // 功能图标统一用 Material 图标，不用 emoji 充当图标（见 UI Design.md §3.6）
    implementation("androidx.compose.material:material-icons-extended")
    // 预测式返回（Android 13+，低版本自动降级）
    implementation("androidx.activity:activity-compose:1.12.4")
    debugImplementation("androidx.compose.ui:ui-tooling")
    implementation("androidx.compose.ui:ui-tooling-preview")

    testImplementation("junit:junit:4.13.2")
    // Android 自带的 org.json 在 JVM 单测里是 stub（调用即抛），
    // 单测需要真实的 org.json 实现。
    testImplementation("org.json:json:20240303")
}
