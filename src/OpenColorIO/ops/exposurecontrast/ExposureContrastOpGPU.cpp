// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the OpenColorIO Project.

#include <algorithm>
#include <cmath>

#include <OpenColorIO/OpenColorIO.h>

#include "Logging.h"
#include "ops/exposurecontrast/ExposureContrastOpGPU.h"
#include "utils/StringUtils.h"


namespace OCIO_NAMESPACE
{
namespace
{

static constexpr char EC_EXPOSURE[] = "exposureVal";
static constexpr char EC_CONTRAST[] = "contrastVal";
static constexpr char EC_GAMMA[]    = "gammaVal";
static constexpr char EC_PIVOT[]    = "pivotVal";

void AddUniform(GpuShaderCreatorRcPtr & shaderCreator,
                DynamicPropertyDoubleRcPtr prop,
                const std::string & name)
{
    GpuShaderCreator::DoubleGetter getDouble = std::bind(&DynamicPropertyDouble::getValue,
                                                        prop.get());
    shaderCreator->addUniform(name.c_str(), getDouble);
    // Declare uniform.
    GpuShaderText stDecl(shaderCreator->getLanguage());
    stDecl.declareUniformFloat(name);
    shaderCreator->addToParameterDeclareShaderCode(stDecl.string().c_str());
}

std::string AddProperty(GpuShaderCreatorRcPtr & shaderCreator,
                        GpuShaderText & st,
                        DynamicPropertyDoubleImplRcPtr prop,
                        const std::string & name)
{
    std::string finalName;

    if(prop->isDynamic() && shaderCreator->getLanguage() != LANGUAGE_OSL_1)
    {
        // Build the name for the uniform. The same type of property should give the same name, so
        // that uniform is declared only once, but multiple instances of the shader code can
        // reference that name.
        // Note: No need to add an index to the name to avoid collisions as the dynamic properties
        // are unique.
        finalName = BuildResourceName(shaderCreator, "exposure_contrast", name);

        // Property is decoupled and added to shader creator.
        auto shaderProp = prop->createEditableCopy();
        DynamicPropertyRcPtr newProp = shaderProp;
        shaderCreator->addDynamicProperty(newProp);
        auto newPropDouble = DynamicPropertyValue::AsDouble(newProp);

        // Uniform is added, connected to the shader creator instance of the dynamic property.
        AddUniform(shaderCreator, newPropDouble, finalName);
    }
    else
    {
        // Declare a local variable to be used by the shader code.
        finalName = name;
        st.declareVar(finalName, (float)prop->getValue());

        if (shaderCreator->getLanguage() == LANGUAGE_OSL_1 && prop->isDynamic())
        {
            std::string msg("The dynamic properties are not yet supported by the 'Open Shading language"\
                            " (OSL)' translation: The '");
            msg += name;
            msg += "' dynamic property is replaced by a local variable.";

            LogWarning(msg);
        }
    }

    return finalName;
}

// Not routed through AddProperty, whose non-dynamic branch would declare a local holding the
// raw pivot and so change the source generated for every existing caller.  Returns the uniform
// name, or empty if the pivot stays a literal.
std::string AddPivotProperty(GpuShaderCreatorRcPtr & shaderCreator,
                             ConstExposureContrastOpDataRcPtr & ec)
{
    auto prop = ec->getPivotProperty();

    if (!prop->isDynamic())
    {
        return "";
    }

    if (shaderCreator->getLanguage() == LANGUAGE_OSL_1)
    {
        LogWarning("The dynamic properties are not yet supported by the 'Open Shading language"
                   " (OSL)' translation: The 'pivotVal' dynamic property is replaced by a "
                   "constant.");
        return "";
    }

    const std::string finalName = BuildResourceName(shaderCreator, "exposure_contrast", EC_PIVOT);

    auto shaderProp = prop->createEditableCopy();
    DynamicPropertyRcPtr newProp = shaderProp;
    shaderCreator->addDynamicProperty(newProp);

    AddUniform(shaderCreator, DynamicPropertyValue::AsDouble(newProp), finalName);

    return finalName;
}

// Declares the converted pivot for the linear styles and returns the float3 expression to use.
std::string AddLinearPivot(GpuShaderText & st,
                           ConstExposureContrastOpDataRcPtr & ec,
                           const std::string & pivotName)
{
    if (pivotName.empty())
    {
        return st.float3Const(std::max(EC::MIN_PIVOT, ec->getPivot()));
    }

    st.newLine() << st.floatDecl("pivot") << " = max( " << EC::MIN_PIVOT << ", "
                                          << pivotName << " );";
    return st.float3Const("pivot");
}

// As above for the video styles, where the pivot is raised to the OETF power.
std::string AddVideoPivot(GpuShaderText & st,
                          ConstExposureContrastOpDataRcPtr & ec,
                          const std::string & pivotName)
{
    if (pivotName.empty())
    {
        return st.float3Const(std::pow(std::max(EC::MIN_PIVOT, ec->getPivot()),
                                       EC::VIDEO_OETF_POWER));
    }

    st.newLine() << st.floatDecl("pivot") << " = pow( max( " << EC::MIN_PIVOT << ", "
                                          << pivotName << " ), "
                                          << EC::VIDEO_OETF_POWER << " );";
    return st.float3Const("pivot");
}

// As above for the log styles.  Declares a scalar rather than a float3, and returns an empty
// name when static since the caller emits the precomputed literal itself.
std::string AddLogPivot(GpuShaderText & st,
                        ConstExposureContrastOpDataRcPtr & ec,
                        const std::string & pivotName)
{
    if (pivotName.empty())
    {
        return "";
    }

    st.newLine() << st.floatDecl("logPivot") << " = max( 0.0, log2( max( " << EC::MIN_PIVOT
                                             << ", " << pivotName << " ) / 0.18 ) * "
                                             << ec->getLogExposureStep() << " + "
                                             << ec->getLogMidGray() << " );";
    return "logPivot";
}

void AddProperties(GpuShaderCreatorRcPtr & shaderCreator,
                   GpuShaderText & st,
                   ConstExposureContrastOpDataRcPtr & ec,
                   std::string & exposureName,
                   std::string & contrastName,
                   std::string & gammaName,
                   std::string & pivotName)
{
    exposureName = AddProperty(shaderCreator, st, ec->getExposureProperty(), EC_EXPOSURE);
    contrastName = AddProperty(shaderCreator, st, ec->getContrastProperty(), EC_CONTRAST);
    gammaName    = AddProperty(shaderCreator, st, ec->getGammaProperty(),    EC_GAMMA);
    pivotName    = AddPivotProperty(shaderCreator, ec);
}

void AddECLinearShader(GpuShaderCreatorRcPtr & shaderCreator,
                       GpuShaderText & st,
                       ConstExposureContrastOpDataRcPtr & ec,
                       const std::string & exposureName,
                       const std::string & contrastName,
                       const std::string & gammaName,
                       const std::string & pivotName)
{
    st.newLine() << st.floatDecl("exposure") << " = pow( 2., " << exposureName << " );";
    st.newLine() << st.floatDecl("contrast") << " = max( " << EC::MIN_CONTRAST << ", "
                                             << "( " << contrastName << " * " << gammaName << " ) );";
    const std::string pivotExpr = AddLinearPivot(st, ec, pivotName);
    st.newLine() << shaderCreator->getPixelName() << ".rgb = "
                 << shaderCreator->getPixelName() << ".rgb * exposure;";

    st.newLine() << "if (contrast != 1.0)";
    st.newLine() << "{";
    {
        st.indent();
        // outColor = pow(max(0, outColor/pivot), contrast) * pivot;
        st.newLine() << shaderCreator->getPixelName() << ".rgb = "
                     <<   "pow( "
                     <<     "max( "
                     <<       st.float3Const(0.0f) << ", "
                     <<       shaderCreator->getPixelName() << ".rgb / " << pivotExpr
                     <<     " ), "
                     <<     st.float3Const("contrast")
                     <<   " ) * "
                     <<   pivotExpr << ";";
        st.dedent();
    }
    st.newLine() << "}";
}

void AddECLinearRevShader(GpuShaderCreatorRcPtr & shaderCreator,
                          GpuShaderText & st,
                          ConstExposureContrastOpDataRcPtr & ec,
                          const std::string & exposureName,
                          const std::string & contrastName,
                          const std::string & gammaName,
                          const std::string & pivotName)
{
    st.newLine() << st.floatDecl("exposure") << " = pow( 2., " << exposureName << " );";
    st.newLine() << st.floatDecl("contrast") << " = 1. / max( " << EC::MIN_CONTRAST << ", "
                                             << "( " << contrastName << " * " << gammaName << " ) );";
    const std::string pivotExpr = AddLinearPivot(st, ec, pivotName);

    st.newLine() << "if (contrast != 1.0)";
    st.newLine() << "{";
    {
        st.indent();
        // outColor = pow(max(0, outColor/pivot), contrast) * pivot;
        st.newLine() << shaderCreator->getPixelName() << ".rgb = "
                     <<   "pow( "
                     <<      "max( "
                     <<         st.float3Const(0.0f) << ", "
                     <<         shaderCreator->getPixelName() << ".rgb / " << pivotExpr
                     <<      " ), "
                     <<      st.float3Const("contrast")
                     <<    " ) * "
                     <<    pivotExpr << ";";
        st.dedent();
    }
    st.newLine() << "}";

    st.newLine() << shaderCreator->getPixelName() << ".rgb = "
                 << shaderCreator->getPixelName() << ".rgb / exposure;";
}

void AddECVideoShader(GpuShaderCreatorRcPtr & shaderCreator,
                      GpuShaderText & st,
                      ConstExposureContrastOpDataRcPtr & ec,
                      const std::string & exposureName,
                      const std::string & contrastName,
                      const std::string & gammaName,
                      const std::string & pivotName)
{
    st.newLine() << st.floatDecl("exposure") << " = pow( pow( 2., " << exposureName << " ), "
                                             << EC::VIDEO_OETF_POWER << ");";
    st.newLine() << st.floatDecl("contrast") << " = max( " << EC::MIN_CONTRAST << ", "
                                             << "( " << contrastName << " * " << gammaName << " ) );";
    const std::string pivotExpr = AddVideoPivot(st, ec, pivotName);
    st.newLine() << shaderCreator->getPixelName() << ".rgb = "
                 << shaderCreator->getPixelName() << ".rgb * exposure;";
    st.newLine() << "if (contrast != 1.0)";
    st.newLine() << "{";
    {
        st.indent();
        // outColor = pow(max(0, outColor/pivot), contrast) * pivot;
        st.newLine() << shaderCreator->getPixelName() << ".rgb = "
                     <<   "pow( "
                     <<     "max( "
                     <<       st.float3Const(0.0f) << ", "
                     <<       shaderCreator->getPixelName() << ".rgb / " << pivotExpr
                     <<     " ), "
                     <<     st.float3Const("contrast")
                     <<   " ) * "
                     <<   pivotExpr << ";";
        st.dedent();
    }
    st.newLine() << "}";
}

void AddECVideoRevShader(GpuShaderCreatorRcPtr & shaderCreator,
                         GpuShaderText & st,
                         ConstExposureContrastOpDataRcPtr & ec,
                         const std::string & exposureName,
                         const std::string & contrastName,
                         const std::string & gammaName,
                         const std::string & pivotName)
{
    st.newLine() << st.floatDecl("exposure") << " = pow( pow( 2., " << exposureName << " ), "
                                             << EC::VIDEO_OETF_POWER << ");";
    st.newLine() << st.floatDecl("contrast") << " = 1. / max( " << EC::MIN_CONTRAST << ", "
                                             << "( " << contrastName << " * " << gammaName << " ) );";
    const std::string pivotExpr = AddVideoPivot(st, ec, pivotName);

    st.newLine() << "if (contrast != 1.0)";
    st.newLine() << "{";
    {
        st.indent();
        // outColor = pow(max(0, outColor/pivot), contrast) * pivot;
        st.newLine() << shaderCreator->getPixelName() << ".rgb = "
                     <<   "pow( "
                     <<     "max( "
                     <<       st.float3Const(0.0f) << ", "
                     <<       shaderCreator->getPixelName() << ".rgb / " << pivotExpr
                     <<     " ), "
                     <<     st.float3Const("contrast")
                     <<   " ) * "
                     <<   pivotExpr << ";";
        st.dedent();
    }
    st.newLine() << "}";

    st.newLine() << shaderCreator->getPixelName() << ".rgb = "
                 << shaderCreator->getPixelName() << ".rgb / exposure;";
}

void AddECLogarithmicShader(GpuShaderCreatorRcPtr & shaderCreator,
                            GpuShaderText & st,
                            ConstExposureContrastOpDataRcPtr & ec,
                            const std::string & exposureName,
                            const std::string & contrastName,
                            const std::string & gammaName,
                            const std::string & pivotName)
{
    st.newLine() << st.floatDecl("exposure") << " = " << exposureName << " * "
                                             << ec->getLogExposureStep() << ";";
    st.newLine() << st.floatDecl("contrast") << " = max( " << EC::MIN_CONTRAST << ", "
                                             << "( " << contrastName << " * " << gammaName << " ) );";

    const std::string logPivotName = AddLogPivot(st, ec, pivotName);
    if (logPivotName.empty())
    {
        double pivot = std::max(EC::MIN_PIVOT, ec->getPivot());
        float logPivot = (float)std::max(0., std::log2(pivot / 0.18) *
                                             ec->getLogExposureStep() +
                                             ec->getLogMidGray());

        st.newLine() << st.floatDecl("offset") << " = ( exposure - " << logPivot
                                               << " ) * contrast + " << logPivot << ";";
    }
    else
    {
        st.newLine() << st.floatDecl("offset") << " = ( exposure - " << logPivotName
                                               << " ) * contrast + " << logPivotName << ";";
    }

    st.newLine() << shaderCreator->getPixelName() << ".rgb = "
                 << shaderCreator->getPixelName() << ".rgb * contrast + offset;";
}

void AddECLogarithmicRevShader(GpuShaderCreatorRcPtr & shaderCreator,
                               GpuShaderText & st,
                               ConstExposureContrastOpDataRcPtr & ec,
                               const std::string & exposureName,
                               const std::string & contrastName,
                               const std::string & gammaName,
                               const std::string & pivotName)
{
    st.newLine() << st.floatDecl("exposure") << " = " << exposureName << " * "
                                             << ec->getLogExposureStep() << ";";
    st.newLine() << st.floatDecl("contrast") << " = max( " << EC::MIN_CONTRAST << ", "
                                             << "( " << contrastName << " * " << gammaName << " ) );";

    const std::string logPivotName = AddLogPivot(st, ec, pivotName);
    if (logPivotName.empty())
    {
        double pivot = std::max(EC::MIN_PIVOT, ec->getPivot());
        float logPivot = (float)std::max(0., std::log2(pivot / 0.18) *
                                             ec->getLogExposureStep() +
                                             ec->getLogMidGray());

        st.newLine() << st.floatDecl("offset") << " = " << logPivot << " - " << logPivot
                                               << " / contrast - exposure;";
    }
    else
    {
        st.newLine() << st.floatDecl("offset") << " = " << logPivotName << " - " << logPivotName
                                               << " / contrast - exposure;";
    }

    st.newLine() << shaderCreator->getPixelName() << ".rgb = "
                 << shaderCreator->getPixelName() << ".rgb / contrast + offset;";
}


}

void GetExposureContrastGPUShaderProgram(GpuShaderCreatorRcPtr & shaderCreator,
                                         ConstExposureContrastOpDataRcPtr & ec)
{
    std::string exposureName;
    std::string contrastName;
    std::string gammaName;
    std::string pivotName;

    GpuShaderText st(shaderCreator->getLanguage());
    st.indent();

    st.newLine() << "";
    st.newLine() << "// Add ExposureContrast '"
                 << ExposureContrastOpData::ConvertStyleToString(ec->getStyle())
                 << "' processing";
    st.newLine() << "";
    st.newLine() << "{";
    st.indent();

    AddProperties(shaderCreator, st, ec,
                  exposureName,
                  contrastName,
                  gammaName,
                  pivotName);

    switch (ec->getStyle())
    {
    case ExposureContrastOpData::STYLE_LINEAR:
        AddECLinearShader(shaderCreator, st, ec, exposureName, contrastName, gammaName, pivotName);
        break;
    case ExposureContrastOpData::STYLE_LINEAR_REV:
        AddECLinearRevShader(shaderCreator, st, ec, exposureName, contrastName,
                              gammaName, pivotName);
        break;
    case ExposureContrastOpData::STYLE_VIDEO:
        AddECVideoShader(shaderCreator, st, ec, exposureName, contrastName, gammaName, pivotName);
        break;
    case ExposureContrastOpData::STYLE_VIDEO_REV:
        AddECVideoRevShader(shaderCreator, st, ec, exposureName, contrastName,
                             gammaName, pivotName);
        break;
    case ExposureContrastOpData::STYLE_LOGARITHMIC:
        AddECLogarithmicShader(shaderCreator, st, ec, exposureName, contrastName,
                                gammaName, pivotName);
        break;
    case ExposureContrastOpData::STYLE_LOGARITHMIC_REV:
        AddECLogarithmicRevShader(shaderCreator, st, ec, exposureName, contrastName,
                                   gammaName, pivotName);
        break;
    }

    st.dedent();
    st.newLine() << "}";

    st.dedent();
    shaderCreator->addToFunctionShaderCode(st.string().c_str());
}

} // namespace OCIO_NAMESPACE
