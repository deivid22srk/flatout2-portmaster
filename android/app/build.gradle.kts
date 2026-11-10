plugins {
    id("com.android.application")
}

android {
    namespace = "com.deivid22srk.flatout2"
    compileSdk = 34
    ndkVersion = "26.3.11579264"

    defaultConfig {
        applicationId = "com.deivid22srk.flatout2"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "1.0.0-android.1"

        ndk {
            // FlatOut 2 recompiled is an AArch64-only binary.
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static")
                cppFlags += "-std=c++17"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = false
            // Signed with the debug key so the APK can be installed without extra setup.
            signingConfig = signingConfigs.getByName("debug")
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }

    lint {
        abortOnError = false
    }
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.7.0")
    // appcompat transitively mixes kotlin-stdlib-jdk7/jdk8 1.6.21 with stdlib 1.8.x;
    // pin the merged artifacts to the same version (official recommendation).
    constraints {
        implementation("org.jetbrains.kotlin:kotlin-stdlib-jdk7:1.8.22") {
            because("kotlin-stdlib 1.8.22 merged jdk7/jdk8 variants")
        }
        implementation("org.jetbrains.kotlin:kotlin-stdlib-jdk8:1.8.22") {
            because("kotlin-stdlib 1.8.22 merged jdk7/jdk8 variants")
        }
    }
}
