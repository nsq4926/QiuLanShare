package com.sanqiu.lanshare.core

/**
 * 对外口径（上架用）：应用名、隐私政策地址、联系邮箱。
 *
 * 改地址只改这一处：隐私同意页、关于页、商店文案都以这里为准。
 * 隐私政策正文见 `docs/15-隐私政策与权限说明.md`，托管在腾讯文档。
 */
object AppLinks {

    /** 隐私政策（腾讯文档，需对外可读；改托管地址只改这里） */
    const val PRIVACY_URL = "https://docs.qq.com/doc/DVmdrQUVCck56UEJ0"

    /**
     * 对外联系邮箱。留空时「关于」页不显示「联系作者」一行。
     * 上架前必须填：商店审核与用户反馈都认这个入口。
     */
    const val CONTACT_EMAIL = ""

    const val APP_NAME = "秋秋快传"
    const val APP_NAME_EN = "QiuLanShare"
}
