// 小柯機器人：照片與影片上傳到 Google 雲端硬碟
// 部署方式見 AI_Voice_Robot/README.md「照片存到 Google 雲端硬碟」
const FOLDER_ID = '你的照片資料夾ID';         // 照片資料夾
const VIDEO_FOLDER_ID = '你的影片資料夾ID';   // 影片資料夾
const TOKEN = 'CHANGE_ME_TOKEN';             // 必須和 config.h 的 GDRIVE_TOKEN 相同

// 機器人以 POST 傳送 base64 檔案：?token=...&name=xxx.jpg&type=photo|video
function doPost(e) {
  if (!e || !e.parameter || e.parameter.token !== TOKEN) {
    return ContentService.createTextOutput('forbidden');
  }
  const isVideo = e.parameter.type === 'video';
  const name = e.parameter.name || ((isVideo ? 'video_' : 'photo_') + new Date().getTime() + (isVideo ? '.avi' : '.jpg'));
  const bytes = Utilities.base64Decode(e.postData.contents);
  const blob = Utilities.newBlob(bytes, isVideo ? 'video/x-msvideo' : 'image/jpeg', name);
  const folder = DriveApp.getFolderById(isVideo ? VIDEO_FOLDER_ID : FOLDER_ID);
  const file = folder.createFile(blob);
  return ContentService.createTextOutput('ok ' + file.getId());
}

// 在編輯器按「執行」這個函式一次，完成授權並確認兩個資料夾都能寫入
function testAccess() {
  Logger.log('照片資料夾：' + DriveApp.getFolderById(FOLDER_ID).getName() + '（可以寫入）');
  Logger.log('影片資料夾：' + DriveApp.getFolderById(VIDEO_FOLDER_ID).getName() + '（可以寫入）');
}
