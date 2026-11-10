pluginManagement {
    repositories {
        // Optional mirror for environments rate-limited by Maven Central
        // (enable with FO2_MAVEN_MIRROR=1). CI keeps the canonical repos.
        if (System.getenv("FO2_MAVEN_MIRROR") != null) {
            maven { url = uri("https://maven.aliyun.com/repository/google") }
            maven { url = uri("https://maven.aliyun.com/repository/public") }
            maven { url = uri("https://maven.aliyun.com/repository/gradle-plugin") }
        }
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode = RepositoriesMode.PREFER_SETTINGS
    repositories {
        if (System.getenv("FO2_MAVEN_MIRROR") != null) {
            maven { url = uri("https://maven.aliyun.com/repository/google") }
            maven { url = uri("https://maven.aliyun.com/repository/public") }
        }
        google()
        mavenCentral()
    }
}

rootProject.name = "FlatOut2-Android"
include(":app")
