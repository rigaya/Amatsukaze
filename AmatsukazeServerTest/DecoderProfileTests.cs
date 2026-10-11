using Amatsukaze.Server;
using System.Runtime.Serialization;
using Xunit;

namespace AmatsukazeServerTest;

public sealed class DecoderProfileTests
{
    [Theory]
    [InlineData(DecoderType.Default, 0, "default", "default")]
    [InlineData(DecoderType.QSV, 1, "QSV", "default")]
    [InlineData(DecoderType.CUVID, 2, "CUVID", "CUVID")]
    [InlineData(DecoderType.VAAPI, 3, "vaapi", "vaapi")]
    public void デコーダーの既存番号とCLI引数を維持する(
        DecoderType decoder, int value, string v2Argument, string legacyArgument)
    {
        Assert.Equal(value, (int)decoder);
        Assert.Equal(v2Argument, decoder.GetDecoderArgument(true));
        Assert.Equal(legacyArgument, decoder.GetDecoderArgument(false));
    }

    [Fact]
    public void VAAPI設定は全コーデックで保存と読み込みを維持する()
    {
        var serializer = new DataContractSerializer(typeof(ProfileSetting));
        using var stream = new MemoryStream();
        serializer.WriteObject(stream, new ProfileSetting
        {
            Mpeg2Decoder = DecoderType.VAAPI,
            H264Deocder = DecoderType.VAAPI,
            HEVCDecoder = DecoderType.VAAPI,
        });
        stream.Position = 0;
        var restored = Assert.IsType<ProfileSetting>(serializer.ReadObject(stream));

        Assert.Equal(DecoderType.VAAPI, restored.Mpeg2Decoder);
        Assert.Equal(DecoderType.VAAPI, restored.H264Deocder);
        Assert.Equal(DecoderType.VAAPI, restored.HEVCDecoder);
        Assert.Equal("VAAPI (Linux)", ProfileSettingExtensions.Mpeg2DecoderList[(int)restored.Mpeg2Decoder]);
        Assert.Equal("VAAPI (Linux)", ProfileSettingExtensions.H264DecoderList[(int)restored.H264Deocder]);
        Assert.Equal("VAAPI (Linux)", ProfileSettingExtensions.HEVCDecoderList[(int)restored.HEVCDecoder]);
    }
}
