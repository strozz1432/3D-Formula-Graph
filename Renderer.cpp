#include "Renderer.h"
#include "Vector3.h"
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define GLFW_STATIC
#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/freeglut.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

#include "exprtk.hpp"

std::string g_formula = "sin(x)*cos(y)";
double g_range_min = -10.0;
double g_range_max = 10.0;
double g_step = 0.5;
bool g_formula_dirty = true;
std::mutex g_formula_mutex;

struct UserVariable {
    std::string name;
    double value;
    double minVal;
    double maxVal;
    bool isDragging;
};

std::vector<UserVariable> g_userVars;
std::string g_consoleInput;
std::vector<std::string> g_consoleHistory;
bool g_consoleActive = true;
int g_cursorPos = 0;
int g_historyScroll = 0;
const int g_visibleLines = 4;

double g_fps = 0.0;
double g_frameTime = 0.0;

struct CachedVertex {
    float x, y, z;
    float r, g, b;
};
std::vector<CachedVertex> g_cachedVertices;
std::vector<int> g_stripStarts;
bool g_cacheValid = false;
GLuint g_displayList = 0;

bool g_isParametric = false;
std::string g_paramX, g_paramY, g_paramZ;
std::string g_paramVar = "t";

struct OrbitCamera {
    float distance = 30.0f;
    float pitch = 20.0f;
    float yaw = -45.0f;
    float sensitivity = 0.3f;
    float scale = 1.0f;
};

bool firstMouse = true;
double lastX, lastY;
int windowWidth = 1000, windowHeight = 700;

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    OrbitCamera* cam = reinterpret_cast<OrbitCamera*>(glfwGetWindowUserPointer(window));
    if (!cam) return;
    
    if (ypos > windowHeight - 120) return;
    
    for (auto& var : g_userVars) {
        if (var.isDragging) {
            double panelX = windowWidth - 200;
            double sliderX = panelX + 10;
            double sliderWidth = 150;
            double t = (xpos - sliderX) / sliderWidth;
            t = std::max(0.0, std::min(1.0, t));
            var.value = var.minVal + t * (var.maxVal - var.minVal);
            g_formula_dirty = true;
            return;
        }
    }
    
    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) != GLFW_PRESS) { firstMouse = true; return; }
    if (firstMouse) { lastX = xpos; lastY = ypos; firstMouse = false; }
    double xoffset = xpos - lastX, yoffset = ypos - lastY;
    lastX = xpos; lastY = ypos;
    cam->yaw += static_cast<float>(xoffset * cam->sensitivity);
    cam->pitch += static_cast<float>(yoffset * cam->sensitivity);
    if (cam->pitch > 89.0f) cam->pitch = 89.0f;
    if (cam->pitch < -89.0f) cam->pitch = -89.0f;
}

void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        double xpos, ypos;
        glfwGetCursorPos(window, &xpos, &ypos);
        
        if (action == GLFW_PRESS) {
            bool clickedConsole = (ypos >= windowHeight - 120);
            g_consoleActive = clickedConsole;
            
            int sliderY = 50;
            for (auto& var : g_userVars) {
                double panelX = windowWidth - 200;
                double sliderX = panelX + 10;
                if (xpos >= sliderX && xpos <= sliderX + 150 && ypos >= sliderY - 5 && ypos <= sliderY + 20) {
                    var.isDragging = true;
                }
                sliderY += 40;
            }
        } else if (action == GLFW_RELEASE) {
            for (auto& var : g_userVars) {
                var.isDragging = false;
            }
        }
    }
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    OrbitCamera* cam = reinterpret_cast<OrbitCamera*>(glfwGetWindowUserPointer(window));
    if (!cam) return;
    cam->distance -= (float)(yoffset * 3.0);
    if (cam->distance < 2.0f) cam->distance = 2.0f;
    if (cam->distance > 200.0f) cam->distance = 200.0f;
}

void processCommand(const std::string& cmd);

void character_callback(GLFWwindow* window, unsigned int codepoint) {
    if (g_consoleActive && codepoint >= 32 && codepoint < 127) {
        g_consoleInput.insert(g_cursorPos, 1, (char)codepoint);
        g_cursorPos++;
    }
}

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    OrbitCamera* cam = reinterpret_cast<OrbitCamera*>(glfwGetWindowUserPointer(window));
    
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        if (g_consoleActive) {
            if (key == GLFW_KEY_BACKSPACE && g_cursorPos > 0) {
                g_consoleInput.erase(g_cursorPos - 1, 1);
                g_cursorPos--;
            }
            else if (key == GLFW_KEY_DELETE && g_cursorPos < (int)g_consoleInput.length()) {
                g_consoleInput.erase(g_cursorPos, 1);
            }
            else if (key == GLFW_KEY_LEFT && g_cursorPos > 0) {
                g_cursorPos--;
            }
            else if (key == GLFW_KEY_RIGHT && g_cursorPos < (int)g_consoleInput.length()) {
                g_cursorPos++;
            }
            else if (key == GLFW_KEY_HOME) {
                g_cursorPos = 0;
            }
            else if (key == GLFW_KEY_END) {
                g_cursorPos = (int)g_consoleInput.length();
            }
            else if (key == GLFW_KEY_ENTER) {
                if (!g_consoleInput.empty()) {
                    g_consoleHistory.push_back("> " + g_consoleInput);
                    processCommand(g_consoleInput);
                    g_consoleInput.clear();
                    g_cursorPos = 0;
                    g_historyScroll = 0;
                }
            }
            else if (key == GLFW_KEY_UP || key == GLFW_KEY_PAGE_UP) {
                int maxScroll = std::max(0, (int)g_consoleHistory.size() - g_visibleLines);
                g_historyScroll = std::min(g_historyScroll + 1, maxScroll);
            }
            else if (key == GLFW_KEY_DOWN || key == GLFW_KEY_PAGE_DOWN) {
                g_historyScroll = std::max(0, g_historyScroll - 1);
            }
        }
        
        if (key == GLFW_KEY_W && cam) {
            cam->scale *= 0.5f;
            if (cam->scale < 0.0625f) cam->scale = 0.0625f;
        }
        if (key == GLFW_KEY_S && cam) {
            cam->scale *= 2.0f;
            if (cam->scale > 16.0f) cam->scale = 16.0f;
        }
    }
}

enum class EquationType {
    EXPLICIT_Z,
    PARAMETRIC_LINE,
    IMPLICIT
};

struct ExprEvaluator {
    typedef exprtk::symbol_table<double> symbol_table_t;
    typedef exprtk::expression<double> expression_t;
    typedef exprtk::parser<double> parser_t;

    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;

    symbol_table_t symbol_table;
    expression_t expression;
    parser_t parser;
    EquationType eqType = EquationType::EXPLICIT_Z;
    std::string originalFormula;
    std::string rightSideFormula;
    expression_t rightSideExpression;
    bool hasRightSideExpression = false;
    bool allVarsEqual = false;

    ExprEvaluator() {
        symbol_table.add_variable("x", X);
        symbol_table.add_variable("y", Y);
        symbol_table.add_variable("z", Z);
        symbol_table.add_constants();
        expression.register_symbol_table(symbol_table);
        rightSideExpression.register_symbol_table(symbol_table);
        parser.settings().disable_all_control_structures();
    }

    void addUserVariable(const std::string& name, double& value) {
        symbol_table.add_variable(name, value);
    }

    bool compile(const std::string& formula) {
        originalFormula = formula;
        std::string processedFormula = formula;
        hasRightSideExpression = false;
        allVarsEqual = false;

        size_t eqCount = 0;
        for (size_t i = 0; i < formula.length(); ++i) {
            if (formula[i] == '=') eqCount++;
        }

        size_t eqPos = formula.find('=');
        if (eqPos != std::string::npos && eqPos > 0 && eqPos < formula.length() - 1) {
            if (eqCount > 1) {
                std::vector<std::string> parts;
                size_t lastPos = 0;
                for (size_t i = 0; i < formula.length(); ++i) {
                    if (formula[i] == '=') {
                        std::string part = formula.substr(lastPos, i - lastPos);
                        part.erase(0, part.find_first_not_of(" \t"));
                        if (!part.empty()) {
                            size_t end = part.find_last_not_of(" \t");
                            if (end != std::string::npos) part = part.substr(0, end + 1);
                        }
                        if (!part.empty()) parts.push_back(part);
                        lastPos = i + 1;
                    }
                }
                std::string lastPart = formula.substr(lastPos);
                lastPart.erase(0, lastPart.find_first_not_of(" \t"));
                if (!lastPart.empty()) {
                    size_t end = lastPart.find_last_not_of(" \t");
                    if (end != std::string::npos) lastPart = lastPart.substr(0, end + 1);
                }
                if (!lastPart.empty()) parts.push_back(lastPart);

                bool allSingleVars = true;
                for (const auto& p : parts) {
                    std::string lower = p;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    if (lower != "x" && lower != "y" && lower != "z") {
                        allSingleVars = false;
                        break;
                    }
                }

                if (allSingleVars && parts.size() >= 2) {
                    eqType = EquationType::PARAMETRIC_LINE;
                    allVarsEqual = true;
                    processedFormula = "0";
                } else if (parts.size() == 3) {
                    int singleVarCount = 0;
                    int exprIdx = -1;
                    std::string singleVars;
                    for (size_t i = 0; i < parts.size(); ++i) {
                        std::string lower = parts[i];
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                        if (lower == "x" || lower == "y" || lower == "z") {
                            singleVarCount++;
                            singleVars += lower[0];
                        } else {
                            exprIdx = (int)i;
                        }
                    }
                    if (singleVarCount == 2 && exprIdx >= 0) {
                        std::string exprLower = parts[exprIdx];
                        std::transform(exprLower.begin(), exprLower.end(), exprLower.begin(), ::tolower);
                        char paramVar = 0;
                        if (exprLower.find('x') != std::string::npos && singleVars.find('x') == std::string::npos) paramVar = 'x';
                        else if (exprLower.find('y') != std::string::npos && singleVars.find('y') == std::string::npos) paramVar = 'y';
                        else if (exprLower.find('z') != std::string::npos && singleVars.find('z') == std::string::npos) paramVar = 'z';
                        
                        if (paramVar != 0) {
                            eqType = EquationType::PARAMETRIC_LINE;
                            allVarsEqual = false;
                            rightSideFormula = parts[exprIdx];
                            rightSideFormula += std::string("|") + paramVar + "|" + singleVars;
                            hasRightSideExpression = parser.compile(parts[exprIdx], rightSideExpression);
                            processedFormula = "0";
                        } else {
                            eqType = EquationType::IMPLICIT;
                            std::string result = "((" + parts[0] + ") - (" + parts[1] + "))^2";
                            for (size_t i = 2; i < parts.size(); ++i) {
                                result += " + ((" + parts[i-1] + ") - (" + parts[i] + "))^2";
                            }
                            processedFormula = result;
                        }
                    } else {
                        eqType = EquationType::IMPLICIT;
                        if (parts.size() >= 2) {
                            std::string result = "((" + parts[0] + ") - (" + parts[1] + "))^2";
                            for (size_t i = 2; i < parts.size(); ++i) {
                                result += " + ((" + parts[i-1] + ") - (" + parts[i] + "))^2";
                            }
                            processedFormula = result;
                        }
                    }
                } else {
                    eqType = EquationType::IMPLICIT;
                    if (parts.size() >= 2) {
                        std::string result = "((" + parts[0] + ") - (" + parts[1] + "))^2";
                        for (size_t i = 2; i < parts.size(); ++i) {
                            result += " + ((" + parts[i-1] + ") - (" + parts[i] + "))^2";
                        }
                        processedFormula = result;
                    }
                }
            } else {
                std::string left = formula.substr(0, eqPos);
                std::string right = formula.substr(eqPos + 1);
                size_t leftStart = left.find_first_not_of(" \t");
                if (leftStart != std::string::npos) {
                    left = left.substr(leftStart);
                    size_t leftEnd = left.find_last_not_of(" \t");
                    if (leftEnd != std::string::npos) {
                        left = left.substr(0, leftEnd + 1);
                    }
                }
                size_t rightStart = right.find_first_not_of(" \t");
                if (rightStart != std::string::npos) {
                    right = right.substr(rightStart);
                    size_t rightEnd = right.find_last_not_of(" \t");
                    if (rightEnd != std::string::npos) {
                        right = right.substr(0, rightEnd + 1);
                    }
                }

                std::string lowerLeft = left;
                std::transform(lowerLeft.begin(), lowerLeft.end(), lowerLeft.begin(), ::tolower);
                std::string lowerRight = right;
                std::transform(lowerRight.begin(), lowerRight.end(), lowerRight.begin(), ::tolower);

                bool hasX = lowerRight.find('x') != std::string::npos;
                bool hasY = lowerRight.find('y') != std::string::npos;
                bool hasZ = lowerRight.find('z') != std::string::npos;

                if (lowerLeft == "z" && hasX && hasY && !hasZ) {
                    eqType = EquationType::EXPLICIT_Z;
                    processedFormula = right;
                } else {
                    eqType = EquationType::IMPLICIT;
                    rightSideFormula = right;
                    hasRightSideExpression = parser.compile(right, rightSideExpression);
                    processedFormula = "(" + left + ") - (" + right + ")";
                }
            }
        } else {
            eqType = EquationType::EXPLICIT_Z;
        }

        return parser.compile(processedFormula, expression);
    }

    double evalRightSide(double x, double y, double z) {
        if (!hasRightSideExpression) return 0.0;
        X = x; Y = y; Z = z;
        return rightSideExpression.value();
    }

    double eval(double x, double y) {
        X = x; Y = y; Z = 0.0;
        return expression.value();
    }

    double evalImplicit(double x, double y, double z) {
        X = x; Y = y; Z = z;
        return expression.value();
    }
};

ExprEvaluator* g_evaluator = nullptr;

void processCommand(const std::string& cmd) {
    std::string trimmed = cmd;
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    if (!trimmed.empty()) {
        size_t end = trimmed.find_last_not_of(" \t");
        if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);
    }
    
    if (trimmed.substr(0, 4) == "var ") {
        std::string rest = trimmed.substr(4);
        size_t eqPos = rest.find('=');
        if (eqPos != std::string::npos) {
            std::string varName = rest.substr(0, eqPos);
            std::string afterEq = rest.substr(eqPos + 1);
            varName.erase(0, varName.find_first_not_of(" \t"));
            if (!varName.empty()) {
                size_t end = varName.find_last_not_of(" \t");
                if (end != std::string::npos) varName = varName.substr(0, end + 1);
            }
            afterEq.erase(0, afterEq.find_first_not_of(" \t"));
            
            double val = 0.0;
            double minV = -10.0;
            double maxV = 10.0;
            
            size_t fromPos = afterEq.find(" from ");
            if (fromPos != std::string::npos) {
                std::string valPart = afterEq.substr(0, fromPos);
                std::string rangePart = afterEq.substr(fromPos + 6);
                try { val = std::stod(valPart); } catch (...) {}
                
                size_t toPos = rangePart.find(" to ");
                if (toPos != std::string::npos) {
                    std::string minStr = rangePart.substr(0, toPos);
                    std::string maxStr = rangePart.substr(toPos + 4);
                    try { minV = std::stod(minStr); } catch (...) {}
                    try { maxV = std::stod(maxStr); } catch (...) {}
                }
            } else {
                std::istringstream iss(afterEq);
                iss >> val;
                double r1, r2;
                if (iss >> r1 >> r2) {
                    minV = r1;
                    maxV = r2;
                } else {
                    minV = val - 10.0;
                    maxV = val + 10.0;
                }
            }
            
            bool found = false;
            for (auto& v : g_userVars) {
                if (v.name == varName) {
                    v.value = val;
                    v.minVal = minV;
                    v.maxVal = maxV;
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                UserVariable newVar;
                newVar.name = varName;
                newVar.value = val;
                newVar.minVal = minV;
                newVar.maxVal = maxV;
                newVar.isDragging = false;
                g_userVars.push_back(newVar);
                
                if (g_evaluator) {
                    g_evaluator->addUserVariable(varName, g_userVars.back().value);
                }
            }
            
            char buf[128];
            snprintf(buf, sizeof(buf), "var %s = %.2f [%.2f to %.2f]", varName.c_str(), val, minV, maxV);
            g_consoleHistory.push_back(buf);
            g_formula_dirty = true;
        }
    }
    else if (trimmed.substr(0, 6) == "range ") {
        std::istringstream iss(trimmed.substr(6));
        double a, b;
        if (iss >> a >> b) {
            g_range_min = a;
            g_range_max = b;
            g_formula_dirty = true;
            g_consoleHistory.push_back("Range: " + std::to_string(a) + " to " + std::to_string(b));
        }
    }
    else if (trimmed.substr(0, 5) == "step ") {
        double s = std::stod(trimmed.substr(5));
        if (s > 0) {
            g_step = s;
            g_formula_dirty = true;
            g_consoleHistory.push_back("Step: " + std::to_string(s));
        }
    }
    else if (trimmed == "help") {
        g_consoleHistory.push_back("Commands:");
        g_consoleHistory.push_back("  <formula>  - set formula (e.g. sin(x)*cos(y))");
        g_consoleHistory.push_back("  param x(t), y(t), z(t)  - parametric curve");
        g_consoleHistory.push_back("  var a = 5  - create slider (default range val-10 to val+10)");
        g_consoleHistory.push_back("  var t = 0 from -31.4 to 31.4  - slider with custom range");
        g_consoleHistory.push_back("  range -5 5 - set x,y,z range");
        g_consoleHistory.push_back("  step 0.5   - set grid step");
        g_consoleHistory.push_back("Functions: sin cos tan asin acos atan exp log sqrt abs pow");
    }
    else if (trimmed.substr(0, 6) == "param ") {
        std::string rest = trimmed.substr(6);
        std::vector<std::string> parts;
        std::string current;
        int parenDepth = 0;
        for (char c : rest) {
            if (c == '(') parenDepth++;
            else if (c == ')') parenDepth--;
            if (c == ',' && parenDepth == 0) {
                parts.push_back(current);
                current.clear();
            } else {
                current += c;
            }
        }
        if (!current.empty()) parts.push_back(current);
        
        for (auto& p : parts) {
            p.erase(0, p.find_first_not_of(" \t"));
            if (!p.empty()) {
                size_t end = p.find_last_not_of(" \t");
                if (end != std::string::npos) p = p.substr(0, end + 1);
            }
        }
        
        if (parts.size() >= 3) {
            g_paramX = parts[0];
            g_paramY = parts[1];
            g_paramZ = parts[2];
            g_isParametric = true;
            g_formula_dirty = true;
            g_consoleHistory.push_back("Parametric: x=" + g_paramX + ", y=" + g_paramY + ", z=" + g_paramZ);
        } else if (parts.size() == 2) {
            g_paramX = parts[0];
            g_paramY = parts[1];
            g_paramZ = "0";
            g_isParametric = true;
            g_formula_dirty = true;
            g_consoleHistory.push_back("Parametric 2D: x=" + g_paramX + ", y=" + g_paramY);
        } else {
            g_consoleHistory.push_back("Usage: param x(t), y(t), z(t)");
        }
    }
    else if (!trimmed.empty()) {
        g_formula = trimmed;
        g_isParametric = false;
        g_formula_dirty = true;
    }
}

std::string g_lastCompileError;

struct ParametricEvaluator {
    typedef exprtk::symbol_table<double> symbol_table_t;
    typedef exprtk::expression<double> expression_t;
    typedef exprtk::parser<double> parser_t;

    double T = 0.0;
    symbol_table_t symbol_table;
    expression_t exprX, exprY, exprZ;
    parser_t parser;
    bool compiled = false;

    ParametricEvaluator() {
        symbol_table.add_variable("t", T);
        symbol_table.add_constants();
        exprX.register_symbol_table(symbol_table);
        exprY.register_symbol_table(symbol_table);
        exprZ.register_symbol_table(symbol_table);
    }

    void addUserVariable(const std::string& name, double& value) {
        symbol_table.add_variable(name, value);
    }

    bool compile(const std::string& xExpr, const std::string& yExpr, const std::string& zExpr) {
        bool ok = true;
        ok = ok && parser.compile(xExpr, exprX);
        ok = ok && parser.compile(yExpr, exprY);
        ok = ok && parser.compile(zExpr, exprZ);
        compiled = ok;
        return ok;
    }

    void eval(double t, double& x, double& y, double& z) {
        T = t;
        x = exprX.value();
        y = exprY.value();
        z = exprZ.value();
    }
};

ParametricEvaluator* g_paramEvaluator = nullptr;

inline bool IsInRange(float val, float rangeMin, float rangeMax) {
    const float MARGIN = 5.0f;
    return val >= rangeMin - MARGIN && val <= rangeMax + MARGIN && std::isfinite(val);
}

inline bool IsVertexValid(float x, float y, float z, float rangeMin, float rangeMax) {
    return IsInRange(x, rangeMin, rangeMax) && 
           IsInRange(y, rangeMin, rangeMax) && 
           IsInRange(z, rangeMin, rangeMax);
}

void BuildParametricDisplayList(ParametricEvaluator& eval, double tMin, double tMax, int numPoints = 2000) {
    if (g_displayList != 0) {
        glDeleteLists(g_displayList, 1);
    }
    g_displayList = glGenLists(1);
    glNewList(g_displayList, GL_COMPILE);
    
    glLineWidth(3.0f);
    bool inLine = false;
    float rangeMinF = (float)g_range_min;
    float rangeMaxF = (float)g_range_max;
    
    for (int i = 0; i <= numPoints; ++i) {
        double t = tMin + (tMax - tMin) * (i / (double)numPoints);
        double x, y, z;
        eval.eval(t, x, y, z);
        
        if (IsVertexValid((float)x, (float)y, (float)z, rangeMinF, rangeMaxF)) {
            if (!inLine) { glBegin(GL_LINE_STRIP); inLine = true; }
            float colorT = (float)i / numPoints;
            glColor3f(1.0f - colorT * 0.5f, 0.3f + colorT * 0.4f, 0.2f + colorT * 0.6f);
            glVertex3f((float)x, (float)y, (float)z);
        } else {
            if (inLine) { glEnd(); inLine = false; }
        }
    }
    if (inLine) glEnd();
    
    glEndList();
    g_cacheValid = true;
}

void DrawParametricCurve() {
    if (g_displayList != 0) {
        glCallList(g_displayList);
    }
}

void BuildSurfaceDisplayList(ExprEvaluator& eval, double rangeMin, double rangeMax, double step) {
    if (g_displayList != 0) {
        glDeleteLists(g_displayList, 1);
    }
    g_displayList = glGenLists(1);
    glNewList(g_displayList, GL_COMPILE);
    
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    float rangeMinF = (float)rangeMin;
    float rangeMaxF = (float)rangeMax;

    for (double x = rangeMin; x < rangeMax; x += step) {
        bool inStrip = false;
        for (double y = rangeMin; y <= rangeMax; y += step) {
            double z1 = eval.eval(x, y);
            double z2 = eval.eval(x + step, y);
            float x1f = (float)x, y1f = (float)y, z1f = (float)z1;
            float x2f = (float)(x + step), y2f = (float)y, z2f = (float)z2;

            bool v1Valid = IsVertexValid(x1f, y1f, z1f, rangeMinF, rangeMaxF);
            bool v2Valid = IsVertexValid(x2f, y2f, z2f, rangeMinF, rangeMaxF);

            if (v1Valid && v2Valid) {
                if (!inStrip) { glBegin(GL_TRIANGLE_STRIP); inStrip = true; }
                glColor3f(0.2f + (float)((z1 + 5.0) / 20.0), 0.4f, 0.7f - (float)((z1 + 5.0) / 40.0));
                glVertex3f(x1f, y1f, z1f);
                glColor3f(0.2f + (float)((z2 + 5.0) / 20.0), 0.4f, 0.7f - (float)((z2 + 5.0) / 40.0));
                glVertex3f(x2f, y2f, z2f);
            } else {
                if (inStrip) { glEnd(); inStrip = false; }
            }
        }
        if (inStrip) glEnd();
    }
    
    glEndList();
    g_cacheValid = true;
}

void DrawSurface() {
    if (g_displayList != 0) {
        glCallList(g_displayList);
    }
}

void BuildImplicitDisplayList(ExprEvaluator& eval, double rangeMin, double rangeMax, double step) {
    if (g_displayList != 0) {
        glDeleteLists(g_displayList, 1);
    }
    g_displayList = glGenLists(1);
    glNewList(g_displayList, GL_COMPILE);
    
    glLineWidth(3.0f);
    glColor3f(1.0f, 0.8f, 0.2f);

    std::string formula = eval.originalFormula;
    std::string cleanFormula = formula;
    cleanFormula.erase(std::remove_if(cleanFormula.begin(), cleanFormula.end(), ::isspace), cleanFormula.end());

    std::string lowerFormula = cleanFormula;
    std::transform(lowerFormula.begin(), lowerFormula.end(), lowerFormula.begin(), ::tolower);

    size_t eqPos = lowerFormula.find('=');
    if (eqPos != std::string::npos && eqPos > 0 && eqPos < lowerFormula.length() - 1) {
        std::string left = lowerFormula.substr(0, eqPos);
        std::string right = lowerFormula.substr(eqPos + 1);
        left.erase(std::remove_if(left.begin(), left.end(), ::isspace), left.end());
        right.erase(std::remove_if(right.begin(), right.end(), ::isspace), right.end());

        if (left.length() == 1 && (left[0] == 'x' || left[0] == 'y' || left[0] == 'z')) {
            char leftVar = left[0];
            bool hasX = right.find('x') != std::string::npos;
            bool hasY = right.find('y') != std::string::npos;
            bool hasZ = right.find('z') != std::string::npos;
            int varCount = (hasX ? 1 : 0) + (hasY ? 1 : 0) + (hasZ ? 1 : 0);

            if (varCount == 2 && eval.hasRightSideExpression) {
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                float rangeMinF = (float)rangeMin;
                float rangeMaxF = (float)rangeMax;

                char var1 = '?', var2 = '?';
                if (hasX && leftVar != 'x') { var1 = 'x'; }
                if (hasY && leftVar != 'y') { if (var1 == '?') var1 = 'y'; else var2 = 'y'; }
                if (hasZ && leftVar != 'z') { if (var1 == '?') var1 = 'z'; else var2 = 'z'; }

                if (leftVar == 'y' && var1 == 'x' && var2 == 'z') {
                    for (double x = rangeMin; x < rangeMax; x += step) {
                        bool inStrip = false;
                        for (double z = rangeMin; z <= rangeMax; z += step) {
                            double y1 = eval.evalRightSide(x, 0.0, z);
                            double y2 = eval.evalRightSide(x + step, 0.0, z);
                            float x1f = (float)x, y1f = (float)y1, z1f = (float)z;
                            float x2f = (float)(x + step), y2f = (float)y2, z2f = (float)z;
                            bool v1Valid = IsVertexValid(x1f, y1f, z1f, rangeMinF, rangeMaxF);
                            bool v2Valid = IsVertexValid(x2f, y2f, z2f, rangeMinF, rangeMaxF);
                            if (v1Valid && v2Valid) {
                                if (!inStrip) { glBegin(GL_TRIANGLE_STRIP); inStrip = true; }
                                glColor3f(0.2f + (float)((y1 + 5.0) / 20.0), 0.4f, 0.7f - (float)((y1 + 5.0) / 40.0));
                                glVertex3f(x1f, y1f, z1f);
                                glColor3f(0.2f + (float)((y2 + 5.0) / 20.0), 0.4f, 0.7f - (float)((y2 + 5.0) / 40.0));
                                glVertex3f(x2f, y2f, z2f);
                            } else {
                                if (inStrip) { glEnd(); inStrip = false; }
                            }
                        }
                        if (inStrip) glEnd();
                    }
                    glEndList();
                    g_cacheValid = true;
                    return;
                }
                else if (leftVar == 'x' && var1 == 'y' && var2 == 'z') {
                    for (double y = rangeMin; y < rangeMax; y += step) {
                        bool inStrip = false;
                        for (double z = rangeMin; z <= rangeMax; z += step) {
                            double x1 = eval.evalRightSide(0.0, y, z);
                            double x2 = eval.evalRightSide(0.0, y + step, z);
                            float x1f = (float)x1, y1f = (float)y, z1f = (float)z;
                            float x2f = (float)x2, y2f = (float)(y + step), z2f = (float)z;
                            bool v1Valid = IsVertexValid(x1f, y1f, z1f, rangeMinF, rangeMaxF);
                            bool v2Valid = IsVertexValid(x2f, y2f, z2f, rangeMinF, rangeMaxF);
                            if (v1Valid && v2Valid) {
                                if (!inStrip) { glBegin(GL_TRIANGLE_STRIP); inStrip = true; }
                                glColor3f(0.2f + (float)((x1 + 5.0) / 20.0), 0.4f, 0.7f - (float)((x1 + 5.0) / 40.0));
                                glVertex3f(x1f, y1f, z1f);
                                glColor3f(0.2f + (float)((x2 + 5.0) / 20.0), 0.4f, 0.7f - (float)((x2 + 5.0) / 40.0));
                                glVertex3f(x2f, y2f, z2f);
                            } else {
                                if (inStrip) { glEnd(); inStrip = false; }
                            }
                        }
                        if (inStrip) glEnd();
                    }
                    glEndList();
                    g_cacheValid = true;
                    return;
                }
                else if (leftVar == 'z' && var1 == 'x' && var2 == 'y') {
                    for (double x = rangeMin; x < rangeMax; x += step) {
                        bool inStrip = false;
                        for (double y = rangeMin; y <= rangeMax; y += step) {
                            double z1 = eval.evalRightSide(x, y, 0.0);
                            double z2 = eval.evalRightSide(x + step, y, 0.0);
                            float x1f = (float)x, y1f = (float)y, z1f = (float)z1;
                            float x2f = (float)(x + step), y2f = (float)y, z2f = (float)z2;
                            bool v1Valid = IsVertexValid(x1f, y1f, z1f, rangeMinF, rangeMaxF);
                            bool v2Valid = IsVertexValid(x2f, y2f, z2f, rangeMinF, rangeMaxF);
                            if (v1Valid && v2Valid) {
                                if (!inStrip) { glBegin(GL_TRIANGLE_STRIP); inStrip = true; }
                                glColor3f(0.2f + (float)((z1 + 5.0) / 20.0), 0.4f, 0.7f - (float)((z1 + 5.0) / 40.0));
                                glVertex3f(x1f, y1f, z1f);
                                glColor3f(0.2f + (float)((z2 + 5.0) / 20.0), 0.4f, 0.7f - (float)((z2 + 5.0) / 40.0));
                                glVertex3f(x2f, y2f, z2f);
                            } else {
                                if (inStrip) { glEnd(); inStrip = false; }
                            }
                        }
                        if (inStrip) glEnd();
                    }
                    glEndList();
                    g_cacheValid = true;
                    return;
                }
            }
            else if (varCount == 1 && eval.hasRightSideExpression) {
                int numPoints = 400;
                float rangeMinF = (float)rangeMin;
                float rangeMaxF = (float)rangeMax;

                char rightVar = '?';
                if (hasX && leftVar != 'x') rightVar = 'x';
                else if (hasY && leftVar != 'y') rightVar = 'y';
                else if (hasZ && leftVar != 'z') rightVar = 'z';

                glLineWidth(3.0f);
                bool inLine = false;

                if (rightVar == 'x' && leftVar == 'y') {
                    for (int i = 0; i <= numPoints; ++i) {
                        double x = rangeMin + (rangeMax - rangeMin) * (i / (double)numPoints);
                        double y = eval.evalRightSide(x, 0.0, 0.0);
                        if (IsVertexValid((float)x, (float)y, 0.0f, rangeMinF, rangeMaxF)) {
                            if (!inLine) { glBegin(GL_LINE_STRIP); inLine = true; }
                            float colorT = (float)i / numPoints;
                            glColor3f(1.0f - colorT * 0.5f, 0.5f + colorT * 0.3f, 0.2f + colorT * 0.6f);
                            glVertex3f((float)x, (float)y, 0.0f);
                        } else {
                            if (inLine) { glEnd(); inLine = false; }
                        }
                    }
                    if (inLine) glEnd();
                    glEndList();
                    g_cacheValid = true;
                    return;
                }
                else if (rightVar == 'x' && leftVar == 'z') {
                    for (int i = 0; i <= numPoints; ++i) {
                        double x = rangeMin + (rangeMax - rangeMin) * (i / (double)numPoints);
                        double z = eval.evalRightSide(x, 0.0, 0.0);
                        if (IsVertexValid((float)x, 0.0f, (float)z, rangeMinF, rangeMaxF)) {
                            if (!inLine) { glBegin(GL_LINE_STRIP); inLine = true; }
                            float colorT = (float)i / numPoints;
                            glColor3f(1.0f - colorT * 0.5f, 0.5f + colorT * 0.3f, 0.2f + colorT * 0.6f);
                            glVertex3f((float)x, 0.0f, (float)z);
                        } else {
                            if (inLine) { glEnd(); inLine = false; }
                        }
                    }
                    if (inLine) glEnd();
                    glEndList();
                    g_cacheValid = true;
                    return;
                }
                else if (rightVar == 'y' && leftVar == 'x') {
                    for (int i = 0; i <= numPoints; ++i) {
                        double y = rangeMin + (rangeMax - rangeMin) * (i / (double)numPoints);
                        double x = eval.evalRightSide(0.0, y, 0.0);
                        if (IsVertexValid((float)x, (float)y, 0.0f, rangeMinF, rangeMaxF)) {
                            if (!inLine) { glBegin(GL_LINE_STRIP); inLine = true; }
                            float colorT = (float)i / numPoints;
                            glColor3f(1.0f - colorT * 0.5f, 0.5f + colorT * 0.3f, 0.2f + colorT * 0.6f);
                            glVertex3f((float)x, (float)y, 0.0f);
                        } else {
                            if (inLine) { glEnd(); inLine = false; }
                        }
                    }
                    if (inLine) glEnd();
                    glEndList();
                    g_cacheValid = true;
                    return;
                }
                else if (rightVar == 'y' && leftVar == 'z') {
                    for (int i = 0; i <= numPoints; ++i) {
                        double y = rangeMin + (rangeMax - rangeMin) * (i / (double)numPoints);
                        double z = eval.evalRightSide(0.0, y, 0.0);
                        if (IsVertexValid(0.0f, (float)y, (float)z, rangeMinF, rangeMaxF)) {
                            if (!inLine) { glBegin(GL_LINE_STRIP); inLine = true; }
                            float colorT = (float)i / numPoints;
                            glColor3f(1.0f - colorT * 0.5f, 0.5f + colorT * 0.3f, 0.2f + colorT * 0.6f);
                            glVertex3f(0.0f, (float)y, (float)z);
                        } else {
                            if (inLine) { glEnd(); inLine = false; }
                        }
                    }
                    if (inLine) glEnd();
                    glEndList();
                    g_cacheValid = true;
                    return;
                }
                else if (rightVar == 'z' && leftVar == 'x') {
                    for (int i = 0; i <= numPoints; ++i) {
                        double z = rangeMin + (rangeMax - rangeMin) * (i / (double)numPoints);
                        double x = eval.evalRightSide(0.0, 0.0, z);
                        if (IsVertexValid((float)x, 0.0f, (float)z, rangeMinF, rangeMaxF)) {
                            if (!inLine) { glBegin(GL_LINE_STRIP); inLine = true; }
                            float colorT = (float)i / numPoints;
                            glColor3f(1.0f - colorT * 0.5f, 0.5f + colorT * 0.3f, 0.2f + colorT * 0.6f);
                            glVertex3f((float)x, 0.0f, (float)z);
                        } else {
                            if (inLine) { glEnd(); inLine = false; }
                        }
                    }
                    if (inLine) glEnd();
                    glEndList();
                    g_cacheValid = true;
                    return;
                }
                else if (rightVar == 'z' && leftVar == 'y') {
                    for (int i = 0; i <= numPoints; ++i) {
                        double z = rangeMin + (rangeMax - rangeMin) * (i / (double)numPoints);
                        double y = eval.evalRightSide(0.0, 0.0, z);
                        if (IsVertexValid(0.0f, (float)y, (float)z, rangeMinF, rangeMaxF)) {
                            if (!inLine) { glBegin(GL_LINE_STRIP); inLine = true; }
                            float colorT = (float)i / numPoints;
                            glColor3f(1.0f - colorT * 0.5f, 0.5f + colorT * 0.3f, 0.2f + colorT * 0.6f);
                            glVertex3f(0.0f, (float)y, (float)z);
                        } else {
                            if (inLine) { glEnd(); inLine = false; }
                        }
                    }
                    if (inLine) glEnd();
                    glEndList();
                    g_cacheValid = true;
                    return;
                }
            }
            else if (varCount == 0 && eval.hasRightSideExpression) {
                float rangeMinF = (float)rangeMin;
                float rangeMaxF = (float)rangeMax;
                double constVal = eval.evalRightSide(0.0, 0.0, 0.0);
                
                if (std::isfinite(constVal) && constVal >= rangeMin && constVal <= rangeMax) {
                    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                    glColor4f(0.8f, 0.6f, 0.2f, 0.7f);
                    
                    if (leftVar == 'x') {
                        glBegin(GL_QUADS);
                        glVertex3f((float)constVal, rangeMinF, rangeMinF);
                        glVertex3f((float)constVal, rangeMaxF, rangeMinF);
                        glVertex3f((float)constVal, rangeMaxF, rangeMaxF);
                        glVertex3f((float)constVal, rangeMinF, rangeMaxF);
                        glEnd();
                    }
                    else if (leftVar == 'y') {
                        glBegin(GL_QUADS);
                        glVertex3f(rangeMinF, (float)constVal, rangeMinF);
                        glVertex3f(rangeMaxF, (float)constVal, rangeMinF);
                        glVertex3f(rangeMaxF, (float)constVal, rangeMaxF);
                        glVertex3f(rangeMinF, (float)constVal, rangeMaxF);
                        glEnd();
                    }
                    else if (leftVar == 'z') {
                        glBegin(GL_QUADS);
                        glVertex3f(rangeMinF, rangeMinF, (float)constVal);
                        glVertex3f(rangeMaxF, rangeMinF, (float)constVal);
                        glVertex3f(rangeMaxF, rangeMaxF, (float)constVal);
                        glVertex3f(rangeMinF, rangeMaxF, (float)constVal);
                        glEnd();
                    }
                }
                glEndList();
                g_cacheValid = true;
                return;
            }
        }
    }

    const double tolerance = step * 0.5;
    double sampleStep = step * 0.5;
    
    double rangeSize = rangeMax - rangeMin;
    int maxSteps = 60;
    if (rangeSize / sampleStep > maxSteps) {
        sampleStep = rangeSize / maxSteps;
    }

    glPointSize(4.0f);
    glBegin(GL_POINTS);
    for (double x = rangeMin; x <= rangeMax; x += sampleStep) {
        for (double y = rangeMin; y <= rangeMax; y += sampleStep) {
            for (double z = rangeMin; z <= rangeMax; z += sampleStep) {
                double value = eval.evalImplicit(x, y, z);
                if (fabs(value) < tolerance) {
                    float colorT = (float)((x - rangeMin) / rangeSize);
                    glColor3f(1.0f - colorT * 0.3f, 0.7f, 0.3f + colorT * 0.4f);
                    glVertex3f((float)x, (float)y, (float)z);
                }
            }
        }
    }
    glEnd();
    
    glEndList();
    g_cacheValid = true;
}

void DrawImplicit() {
    if (g_displayList != 0) {
        glCallList(g_displayList);
    }
}

void DrawAxes(float axisMax, float scale) {
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    glColor3f(1, 0, 0); glVertex3f(-axisMax, 0, 0); glVertex3f(axisMax, 0, 0);
    glColor3f(0, 1, 0); glVertex3f(0, -axisMax, 0); glVertex3f(0, axisMax, 0);
    glColor3f(0, 0, 1); glVertex3f(0, 0, -axisMax); glVertex3f(0, 0, axisMax);
    glEnd();

    float tickInterval = scale;
    if (tickInterval < 0.1f) tickInterval = 0.1f;
    if (tickInterval > 10.0f) tickInterval = 10.0f;

    glLineWidth(1.0f);
    glBegin(GL_LINES);
    float tickSize = 0.15f;
    for (double i = -axisMax; i <= axisMax; i += tickInterval) {
        if (fabs(i) < 1e-6) continue;
        glColor3f(1, 0, 0); glVertex3f((float)i, -tickSize, 0); glVertex3f((float)i, tickSize, 0);
        glColor3f(0, 1, 0); glVertex3f(-tickSize, (float)i, 0); glVertex3f(tickSize, (float)i, 0);
        glColor3f(0, 0, 1); glVertex3f(0, -tickSize, (float)i); glVertex3f(0, tickSize, (float)i);
    }
    glEnd();

    float labelOffset = 0.4f;
    char buf[32];
    
    glColor3f(1, 0.3f, 0.3f);
    for (double i = -axisMax; i <= axisMax; i += tickInterval) {
        if (fabs(i) < 1e-6) continue;
        snprintf(buf, sizeof(buf), "%.3g", i);
        glRasterPos3f((float)i, labelOffset, 0.0f);
        for (char* c = buf; *c != 0; ++c) glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12, *c);
    }
    
    glColor3f(0.3f, 1, 0.3f);
    for (double i = -axisMax; i <= axisMax; i += tickInterval) {
        if (fabs(i) < 1e-6) continue;
        snprintf(buf, sizeof(buf), "%.3g", i);
        glRasterPos3f(labelOffset, (float)i, 0.0f);
        for (char* c = buf; *c != 0; ++c) glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12, *c);
    }
    
    glColor3f(0.3f, 0.3f, 1);
    for (double i = -axisMax; i <= axisMax; i += tickInterval) {
        if (fabs(i) < 1e-6) continue;
        snprintf(buf, sizeof(buf), "%.3g", i);
        glRasterPos3f(0.0f, labelOffset, (float)i);
        for (char* c = buf; *c != 0; ++c) glutBitmapCharacter(GLUT_BITMAP_HELVETICA_12, *c);
    }
    
    glColor3f(1, 0.2f, 0.2f);
    glRasterPos3f(axisMax + 0.5f, 0.0f, 0.0f);
    glutBitmapCharacter(GLUT_BITMAP_HELVETICA_18, 'X');
    
    glColor3f(0.2f, 1, 0.2f);
    glRasterPos3f(0.0f, axisMax + 0.5f, 0.0f);
    glutBitmapCharacter(GLUT_BITMAP_HELVETICA_18, 'Y');
    
    glColor3f(0.2f, 0.2f, 1);
    glRasterPos3f(0.0f, 0.0f, axisMax + 0.5f);
    glutBitmapCharacter(GLUT_BITMAP_HELVETICA_18, 'Z');
}

void DrawText(float x, float y, const std::string& text, void* font = GLUT_BITMAP_HELVETICA_12) {
    glRasterPos2f(x, y);
    for (char c : text) {
        glutBitmapCharacter(font, c);
    }
}

void DrawUI() {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, windowWidth, windowHeight, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);

    glColor4f(0.1f, 0.1f, 0.15f, 0.9f);
    glBegin(GL_QUADS);
    glVertex2f(0, windowHeight - 120);
    glVertex2f(windowWidth, windowHeight - 120);
    glVertex2f(windowWidth, windowHeight);
    glVertex2f(0, windowHeight);
    glEnd();

    if (g_consoleActive) {
        glColor3f(0.5f, 0.7f, 1.0f);
    } else {
        glColor3f(0.3f, 0.3f, 0.4f);
    }
    glBegin(GL_LINE_LOOP);
    glVertex2f(10, windowHeight - 110);
    glVertex2f(windowWidth - 10, windowHeight - 110);
    glVertex2f(windowWidth - 10, windowHeight - 10);
    glVertex2f(10, windowHeight - 10);
    glEnd();

    glColor3f(0.8f, 0.8f, 0.8f);
    int historyY = windowHeight - 100;
    int totalLines = (int)g_consoleHistory.size();
    int endIdx = std::max(0, totalLines - g_historyScroll);
    int startIdx = std::max(0, endIdx - g_visibleLines);
    for (int i = startIdx; i < endIdx; ++i) {
        DrawText(20, historyY, g_consoleHistory[i]);
        historyY += 16;
    }
    
    if (totalLines > g_visibleLines) {
        glColor3f(0.5f, 0.5f, 0.6f);
        char scrollInfo[32];
        int maxScroll = std::max(0, totalLines - g_visibleLines);
        snprintf(scrollInfo, sizeof(scrollInfo), "[%d/%d]", maxScroll - g_historyScroll + 1, maxScroll + 1);
        DrawText(windowWidth - 80, windowHeight - 100, scrollInfo);
    }

    glColor3f(0.2f, 0.2f, 0.25f);
    glBegin(GL_QUADS);
    glVertex2f(15, windowHeight - 35);
    glVertex2f(windowWidth - 15, windowHeight - 35);
    glVertex2f(windowWidth - 15, windowHeight - 15);
    glVertex2f(15, windowHeight - 15);
    glEnd();

    glColor3f(1.0f, 1.0f, 1.0f);
    std::string inputDisplay = "> " + g_consoleInput;
    DrawText(20, windowHeight - 20, inputDisplay);

    if (g_consoleActive) {
        static int blinkCounter = 0;
        blinkCounter++;
        if ((blinkCounter / 50) % 2 == 0) {
            std::string beforeCursor = "> " + g_consoleInput.substr(0, g_cursorPos);
            float cursorX = 20;
            for (char c : beforeCursor) {
                cursorX += glutBitmapWidth(GLUT_BITMAP_HELVETICA_12, c);
            }
            glBegin(GL_LINES);
            glVertex2f(cursorX, windowHeight - 32);
            glVertex2f(cursorX, windowHeight - 18);
            glEnd();
        }
    }

    if (!g_userVars.empty()) {
        float panelX = windowWidth - 200;
        glColor4f(0.15f, 0.15f, 0.2f, 0.9f);
        glBegin(GL_QUADS);
        glVertex2f(panelX, 10);
        glVertex2f(windowWidth - 10, 10);
        glVertex2f(windowWidth - 10, 30 + g_userVars.size() * 40);
        glVertex2f(panelX, 30 + g_userVars.size() * 40);
        glEnd();

        glColor3f(0.9f, 0.9f, 0.9f);
        DrawText(panelX + 10, 25, "Variables", GLUT_BITMAP_HELVETICA_12);

        int sliderY = 50;
        for (auto& var : g_userVars) {
            glColor3f(0.8f, 0.8f, 0.8f);
            char label[64];
            snprintf(label, sizeof(label), "%s: %.2f", var.name.c_str(), var.value);
            DrawText(panelX + 10, sliderY, label);

            glColor3f(0.3f, 0.3f, 0.4f);
            glBegin(GL_QUADS);
            glVertex2f(panelX + 10, sliderY + 5);
            glVertex2f(panelX + 160, sliderY + 5);
            glVertex2f(panelX + 160, sliderY + 15);
            glVertex2f(panelX + 10, sliderY + 15);
            glEnd();

            float t = (float)((var.value - var.minVal) / (var.maxVal - var.minVal));
            t = std::max(0.0f, std::min(1.0f, t));
            float handleX = panelX + 10 + t * 150;

            glColor3f(0.9f, 0.6f, 0.2f);
            glBegin(GL_QUADS);
            glVertex2f(handleX - 5, sliderY + 3);
            glVertex2f(handleX + 5, sliderY + 3);
            glVertex2f(handleX + 5, sliderY + 17);
            glVertex2f(handleX - 5, sliderY + 17);
            glEnd();

            sliderY += 40;
        }
    }

    glColor3f(0.6f, 0.6f, 0.6f);
    DrawText(10, 20, "W/S: scale | Scroll: zoom | Drag: rotate | Type 'help' for commands");

    glColor4f(0.8f, 0.8f, 0.8f, 0.7f);
    char fpsText[64];
    snprintf(fpsText, sizeof(fpsText), "%.1f FPS (%.2f ms)", g_fps, g_frameTime * 1000.0);
    DrawText(windowWidth - 140, 20, fpsText);

    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

int Renderer() {
    int argc = 1;
    char* argv[1] = { (char*)"app" };
    glutInit(&argc, argv);

    if (!glfwInit()) { std::cerr << "Failed to init GLFW\n"; return -1; }
    GLFWwindow* window = glfwCreateWindow(windowWidth, windowHeight, "3D Formula Grapher", nullptr, nullptr);
    if (!window) { std::cerr << "Failed to create window\n"; glfwTerminate(); return -1; }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(60.0, windowWidth / (double)windowHeight, 0.1, 1000.0);
    glMatrixMode(GL_MODELVIEW);

    OrbitCamera cam;
    glfwSetWindowUserPointer(window, &cam);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetCharCallback(window, character_callback);
    glfwSetKeyCallback(window, key_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    ExprEvaluator evaluator;
    g_evaluator = &evaluator;
    ParametricEvaluator paramEval;
    g_paramEvaluator = &paramEval;
    bool hasCompiled = false;
    std::string lastFormula;
    std::string lastParamX, lastParamY, lastParamZ;

    hasCompiled = evaluator.compile(g_formula);
    if (!hasCompiled) std::cerr << "Initial compile failed for: " << g_formula << std::endl;
    g_formula_dirty = false;

    g_consoleHistory.push_back("3D Formula Grapher - Type 'help' for commands");
    g_consoleHistory.push_back("Current: " + g_formula);

    auto lastFrameTime = std::chrono::high_resolution_clock::now();
    double fpsAccum = 0.0;
    int frameCount = 0;

    while (!glfwWindowShouldClose(window)) {
        auto currentTime = std::chrono::high_resolution_clock::now();
        g_frameTime = std::chrono::duration<double>(currentTime - lastFrameTime).count();
        lastFrameTime = currentTime;
        
        fpsAccum += g_frameTime;
        frameCount++;
        if (fpsAccum >= 0.5) {
            g_fps = frameCount / fpsAccum;
            fpsAccum = 0.0;
            frameCount = 0;
        }

        glfwGetWindowSize(window, &windowWidth, &windowHeight);

        if (g_formula_dirty) {
            for (auto& var : g_userVars) {
                evaluator.addUserVariable(var.name, var.value);
                paramEval.addUserVariable(var.name, var.value);
            }
            
            g_cacheValid = false;
            
            if (g_isParametric) {
                if (g_paramX != lastParamX || g_paramY != lastParamY || g_paramZ != lastParamZ) {
                    bool ok = paramEval.compile(g_paramX, g_paramY, g_paramZ);
                    if (!ok) {
                        hasCompiled = false;
                        g_consoleHistory.push_back("Error: Invalid parametric formula");
                    } else {
                        lastParamX = g_paramX;
                        lastParamY = g_paramY;
                        lastParamZ = g_paramZ;
                        hasCompiled = true;
                    }
                } else {
                    hasCompiled = paramEval.compiled;
                }
            } else {
                if (g_formula != lastFormula) {
                    bool ok = evaluator.compile(g_formula);
                    if (!ok) {
                        hasCompiled = false;
                        g_consoleHistory.push_back("Error: Invalid formula '" + g_formula + "'");
                    } else {
                        lastFormula = g_formula;
                        hasCompiled = true;
                        g_consoleHistory.push_back("OK: " + g_formula);
                    }
                }
            }
            g_formula_dirty = false;
        }

        glViewport(0, 0, windowWidth, windowHeight);
        glClearColor(0.1f, 0.12f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        gluPerspective(60.0, windowWidth / (double)windowHeight, 0.1, 1000.0);
        glMatrixMode(GL_MODELVIEW);

        glLoadIdentity();
        float radYaw = cam.yaw * 3.14159265f / 180.0f;
        float radPitch = cam.pitch * 3.14159265f / 180.0f;
        float camX = cam.distance * cos(radPitch) * cos(radYaw);
        float camY = cam.distance * sin(radPitch);
        float camZ = cam.distance * cos(radPitch) * sin(radYaw);
        gluLookAt(camX, camY, camZ, 0, 0, 0, 0, 1, 0);

        float axisMax = std::max((float)fabs(g_range_min), (float)fabs(g_range_max));
        if (axisMax < 1.0f) axisMax = 1.0f;
        DrawAxes(axisMax, cam.scale);

        if (hasCompiled) {
            if (!g_cacheValid) {
                if (g_isParametric) {
                    double tMin = g_range_min;
                    double tMax = g_range_max;
                    for (auto& var : g_userVars) {
                        if (var.name == "t") {
                            tMin = var.minVal;
                            tMax = var.maxVal;
                            break;
                        }
                    }
                    BuildParametricDisplayList(paramEval, tMin, tMax, 2000);
                } else if (evaluator.eqType == EquationType::PARAMETRIC_LINE) {
                    if (g_displayList != 0) glDeleteLists(g_displayList, 1);
                    g_displayList = glGenLists(1);
                    glNewList(g_displayList, GL_COMPILE);
                    glLineWidth(3.0f);
                    glBegin(GL_LINE_STRIP);
                    int numPoints = 400;
                    float rangeMinF = (float)g_range_min;
                    float rangeMaxF = (float)g_range_max;
                    
                    char paramVar = 0;
                    std::string singleVars;
                    if (!evaluator.allVarsEqual && evaluator.hasRightSideExpression) {
                        size_t p1 = evaluator.rightSideFormula.find('|');
                        if (p1 != std::string::npos) {
                            size_t p2 = evaluator.rightSideFormula.find('|', p1 + 1);
                            if (p2 != std::string::npos) {
                                paramVar = evaluator.rightSideFormula[p1 + 1];
                                singleVars = evaluator.rightSideFormula.substr(p2 + 1);
                            }
                        }
                    }
                    
                    for (int i = 0; i <= numPoints; ++i) {
                        double t = g_range_min + (g_range_max - g_range_min) * (i / (double)numPoints);
                        float px, py, pz;
                        if (evaluator.allVarsEqual) {
                            px = py = pz = (float)t;
                        } else if (paramVar != 0) {
                            double xv = (paramVar == 'x') ? t : 0;
                            double yv = (paramVar == 'y') ? t : 0;
                            double zv = (paramVar == 'z') ? t : 0;
                            double exprVal = evaluator.evalRightSide(xv, yv, zv);
                            
                            px = (singleVars.find('x') != std::string::npos) ? (float)exprVal : (paramVar == 'x' ? (float)t : (float)exprVal);
                            py = (singleVars.find('y') != std::string::npos) ? (float)exprVal : (paramVar == 'y' ? (float)t : (float)exprVal);
                            pz = (singleVars.find('z') != std::string::npos) ? (float)exprVal : (paramVar == 'z' ? (float)t : (float)exprVal);
                        } else {
                            px = py = pz = (float)t;
                        }
                        if (px >= rangeMinF - 5 && px <= rangeMaxF + 5 &&
                            py >= rangeMinF - 5 && py <= rangeMaxF + 5 &&
                            pz >= rangeMinF - 5 && pz <= rangeMaxF + 5) {
                            float colorT = (float)i / numPoints;
                            glColor3f(1.0f - colorT * 0.5f, 0.6f + colorT * 0.2f, 0.2f + colorT * 0.5f);
                            glVertex3f(px, py, pz);
                        }
                    }
                    glEnd();
                    glEndList();
                    g_cacheValid = true;
                } else if (evaluator.eqType == EquationType::IMPLICIT) {
                    BuildImplicitDisplayList(evaluator, g_range_min, g_range_max, g_step);
                } else {
                    BuildSurfaceDisplayList(evaluator, g_range_min, g_range_max, g_step);
                }
            }
            
            if (g_displayList != 0) {
                glCallList(g_displayList);
            }
        }

        DrawUI();

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    if (g_displayList != 0) {
        glDeleteLists(g_displayList, 1);
        g_displayList = 0;
    }
    g_evaluator = nullptr;
    g_paramEvaluator = nullptr;
    glfwTerminate();
    return 0;
}
