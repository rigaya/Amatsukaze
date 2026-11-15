using System;

namespace Amatsukaze.Server.Rest
{
    public static class FilterPreviewProfile
    {
        public static ProfileSetting Apply(ProfileSetting source, ProfileSetting edited, string name)
        {
            if (source == null || edited == null) throw new ArgumentNullException();
            var result = ServerSupport.DeepCopy(source);
            result.Name = name;
            result.FilterOption = edited.FilterOption;
            result.FilterSetting = edited.FilterSetting == null ? null : ServerSupport.DeepCopy(edited.FilterSetting);
            result.EncoderFilterSetting = edited.EncoderFilterSetting == null ? null : ServerSupport.DeepCopy(edited.EncoderFilterSetting);
            result.QSVEncFilterOption = edited.QSVEncFilterOption;
            result.NVEncFilterOption = edited.NVEncFilterOption;
            result.VCEEncFilterOption = edited.VCEEncFilterOption;
            return result;
        }
    }
}
